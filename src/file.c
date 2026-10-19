#include "common.h"

#include "file.h"
#include "progress.h"
#include "resume.h"
#include "transfer.h"
#include "util.h"
#include "verify.h"

static int set_file_metadata(int fd, const struct stat *source,
                             const char *path, bool preserve)
{
    if (!preserve) {
        return 0;
    }
    if (fchmod(fd, source->st_mode & 07777) < 0) {
        copy_print_error(path, "cannot preserve permissions for");
        return -1;
    }
    struct timespec times[2] = {source->st_atim, source->st_mtim};
    if (futimens(fd, times) < 0) {
        copy_print_error(path, "cannot preserve timestamps for");
        return -1;
    }
    return 0;
}

static int reject_dangling_destination(const char *destination)
{
    struct stat link_stat;
    if (lstat(destination, &link_stat) < 0) {
        if (errno == ENOENT) return 0;
        copy_print_error(destination, "cannot inspect");
        return -1;
    }
    if (!S_ISLNK(link_stat.st_mode)) return 0;

    struct stat target_stat;
    if (stat(destination, &target_stat) == 0) return 0;
    if (errno == ENOENT) {
        fprintf(stderr,
                "%s: refusing to follow dangling destination symbolic link '%s'\n",
                program_name, destination);
        return -1;
    }
    copy_print_error(destination, "cannot inspect symbolic link target for");
    return -1;
}

int copy_regular_file(const char *source, const char *destination,
                             const struct stat *source_stat,
                             const Options *options, Progress *progress)
{
    if (reject_dangling_destination(destination) < 0) return -1;

    if (options->dry_run) {
        struct stat destination_lstat;
        bool destination_exists = lstat(destination, &destination_lstat) == 0;
        if (!destination_exists && errno != ENOENT) {
            copy_print_error(destination, "cannot inspect");
            return -1;
        }
        struct stat destination_stat;
        bool destination_is_directory = false;
        if (stat(destination, &destination_stat) == 0) {
            if (copy_same_file(source_stat, &destination_stat)) {
                fprintf(stderr, "%s: '%s' and '%s' are the same file\n",
                        program_name, source, destination);
                return -1;
            }
            destination_is_directory = S_ISDIR(destination_stat.st_mode);
        } else if (errno != ENOENT) {
            copy_print_error(destination, "cannot inspect");
            return -1;
        }
        if (options->no_clobber && destination_exists) {
            copy_print_dry_run("skip existing destination", destination, NULL);
            copy_progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
            copy_progress_mark_file(progress);
            return 0;
        }
        if (destination_is_directory) {
            fprintf(stderr, "%s: cannot overwrite directory '%s' with a file\n",
                    program_name, destination);
            return -1;
        }
        if (options->resume && destination_exists &&
            (S_ISLNK(destination_lstat.st_mode) ||
             !S_ISREG(destination_lstat.st_mode))) {
            fprintf(stderr,
                    "%s: --resume requires a regular-file destination: '%s'\n",
                    program_name, destination);
            return -1;
        }
        copy_print_dry_run("copy", source, destination);
        copy_progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
        copy_progress_mark_file(progress);
        return 0;
    }

    int source_fd = open(source, O_RDONLY | O_NOFOLLOW);
    if (source_fd < 0) {
        copy_print_error(source, "cannot open");
        return -1;
    }

    struct stat opened_stat;
    if (fstat(source_fd, &opened_stat) < 0) {
        copy_print_error(source, "cannot inspect");
        close(source_fd);
        return -1;
    }
    if (!S_ISREG(opened_stat.st_mode)) {
        errno = EINVAL;
        copy_print_error(source, "not a regular file");
        close(source_fd);
        return -1;
    }

    struct stat destination_stat;
    if (stat(destination, &destination_stat) == 0) {
        if (copy_same_file(&opened_stat, &destination_stat)) {
            fprintf(stderr, "%s: '%s' and '%s' are the same file\n",
                    program_name, source, destination);
            close(source_fd);
            return -1;
        }
        if (options->verify && !S_ISREG(destination_stat.st_mode)) {
            fprintf(stderr,
                    "%s: --verify requires a regular-file destination: '%s'\n",
                    program_name, destination);
            close(source_fd);
            return -1;
        }
    } else if (errno != ENOENT) {
        copy_print_error(destination, "cannot inspect");
        close(source_fd);
        return -1;
    }

    if (options->resume) {
        return copy_regular_file_resumable(source, destination, source_fd,
                                           &opened_stat, options, progress,
                                           set_file_metadata);
    }

    int flags = (options->verify ? O_RDWR : O_WRONLY) | O_CREAT | O_CLOEXEC;
    if (options->no_clobber) {
        flags |= O_EXCL;
    } else {
        flags |= O_TRUNC;
    }
    int destination_fd = open(destination, flags, source_stat->st_mode & 0777);
    if (destination_fd < 0) {
        if (options->no_clobber && errno == EEXIST) {
            close(source_fd);
            copy_progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
            copy_progress_mark_file(progress);
            return 0;
        }
        copy_print_error(destination, "cannot create");
        close(source_fd);
        return -1;
    }

    struct stat destination_opened_stat;
    if (fstat(destination_fd, &destination_opened_stat) < 0) {
        copy_print_error(destination, "cannot inspect");
        close(destination_fd);
        close(source_fd);
        return -1;
    }
    if (options->verify && !S_ISREG(destination_opened_stat.st_mode)) {
        fprintf(stderr,
                "%s: --verify requires a regular-file destination: '%s'\n",
                program_name, destination);
        close(destination_fd);
        close(source_fd);
        return -1;
    }

    int result = S_ISREG(destination_opened_stat.st_mode)
                     ? copy_transfer_file_contents(source_fd, destination_fd, 0,
                                          opened_stat.st_size, source,
                                          destination, progress)
                     : copy_transfer_stream_contents(source_fd, destination_fd, source,
                                            destination, progress);

    if (result == 0 && options->verify &&
        (copy_prepare_disk_verification(source_fd, destination_fd, source,
                                   destination) < 0 ||
         copy_verify_contents(source_fd, destination_fd, source, destination) < 0)) {
        result = -1;
    }
    if (result == 0 && S_ISREG(destination_opened_stat.st_mode) &&
        set_file_metadata(destination_fd, source_stat, destination,
                          options->preserve) < 0) {
        result = -1;
    }
    if (close(destination_fd) < 0 && result == 0) {
        copy_print_error(destination, "cannot close");
        result = -1;
    }
    if (close(source_fd) < 0 && result == 0) {
        copy_print_error(source, "cannot close");
        result = -1;
    }
    if (result == 0) {
        copy_progress_mark_file(progress);
    }
    if (result == 0 && options->verbose) {
        printf("'%s' -> '%s'\n", source, destination);
    }
    return result;
}

int copy_symlink(const char *source, const char *destination,
                        const struct stat *source_stat, const Options *options,
                        Progress *progress)
{
    if (options->dry_run) {
        struct stat existing;
        if (lstat(destination, &existing) == 0) {
            if (options->no_clobber) {
                copy_print_dry_run("skip existing destination", destination, NULL);
                copy_progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
                copy_progress_mark_file(progress);
                return 0;
            }
            if (S_ISDIR(existing.st_mode) && !S_ISLNK(existing.st_mode)) {
                fprintf(stderr,
                        "%s: refusing to replace directory '%s' with a link\n",
                        program_name, destination);
                return -1;
            }
        } else if (errno != ENOENT) {
            copy_print_error(destination, "cannot inspect");
            return -1;
        }
        copy_print_dry_run("copy symbolic link", source, destination);
        copy_progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
        copy_progress_mark_file(progress);
        return 0;
    }

    size_t capacity = source_stat->st_size > 0
                          ? (size_t)source_stat->st_size + 1
                          : 256;
    char *target = NULL;
    for (;;) {
        target = malloc(capacity + 1);
        if (target == NULL) {
            copy_print_error(source, "cannot allocate symbolic link target for");
            return -1;
        }
        ssize_t length = readlink(source, target, capacity);
        if (length < 0) {
            copy_print_error(source, "cannot read symbolic link");
            free(target);
            return -1;
        }
        if ((size_t)length < capacity) {
            target[length] = '\0';
            break;
        }
        free(target);
        target = NULL;
        if (capacity > SIZE_MAX / 2) {
            errno = ENAMETOOLONG;
            copy_print_error(source, "symbolic link target is too long for");
            return -1;
        }
        capacity *= 2;
    }

    struct stat existing;
    if (lstat(destination, &existing) == 0) {
        if (options->no_clobber) {
            free(target);
            copy_progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
            copy_progress_mark_file(progress);
            return 0;
        }
        if (S_ISDIR(existing.st_mode) && !S_ISLNK(existing.st_mode)) {
            fprintf(stderr, "%s: refusing to replace directory '%s' with a link\n",
                    program_name, destination);
            free(target);
            return -1;
        }
        if (unlink(destination) < 0) {
            copy_print_error(destination, "cannot replace");
            free(target);
            return -1;
        }
    } else if (errno != ENOENT) {
        copy_print_error(destination, "cannot inspect");
        free(target);
        return -1;
    }

    if (symlink(target, destination) < 0) {
        copy_print_error(destination, "cannot create symbolic link");
        free(target);
        return -1;
    }
    free(target);

    if (options->preserve) {
        struct timespec times[2] = {source_stat->st_atim, source_stat->st_mtim};
        if (utimensat(AT_FDCWD, destination, times, AT_SYMLINK_NOFOLLOW) < 0) {
            copy_print_error(destination, "cannot preserve symbolic link timestamps for");
            return -1;
        }
    }
    if (options->verbose) {
        printf("'%s' -> '%s' (symbolic link)\n", source, destination);
    }
    copy_progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
    copy_progress_mark_file(progress);
    return 0;
}
