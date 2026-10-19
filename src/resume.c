#include "common.h"

#include "progress.h"
#include "resume.h"
#include "transfer.h"
#include "util.h"
#include "verify.h"

static char *resume_temporary_path(const char *destination)
{
    char *copy = strdup(destination);
    if (copy == NULL) return NULL;

    char *slash = strrchr(copy, '/');
    const char *parent = ".";
    if (slash != NULL) {
        if (slash == copy) {
            slash[1] = '\0';
            parent = copy;
        } else {
            *slash = '\0';
            parent = copy;
        }
    }

    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *byte = (const unsigned char *)destination;
         *byte != '\0'; byte++) {
        hash ^= *byte;
        hash *= UINT64_C(1099511628211);
    }
    char temporary_name[40];
    snprintf(temporary_name, sizeof temporary_name,
             ".copy-resume-%016" PRIx64, hash);
    char *result = copy_join_path(parent, temporary_name);
    free(copy);
    return result;
}

static int file_prefix_matches(int source_fd, int partial_fd, off_t length,
                               const char *source, const char *partial_path)
{
    unsigned char source_buffer[128 * 1024];
    unsigned char partial_buffer[128 * 1024];
    off_t offset = 0;

    while (offset < length) {
        size_t requested = (uintmax_t)(length - offset) > sizeof source_buffer
                               ? sizeof source_buffer
                               : (size_t)(length - offset);
        ssize_t source_count = copy_read_block_at(source_fd, source_buffer, requested,
                                             offset, source,
                                             "cannot read while resuming");
        if (source_count < 0) return -1;
        ssize_t partial_count = copy_read_block_at(
            partial_fd, partial_buffer, requested, offset, partial_path,
            "cannot read resume file");
        if (partial_count < 0) return -1;
        if (source_count != (ssize_t)requested ||
            partial_count != source_count ||
            memcmp(source_buffer, partial_buffer, (size_t)source_count) != 0) {
            return 0;
        }
        offset += source_count;
    }
    return 1;
}

static bool same_source_version(const struct stat *before,
                                const struct stat *after)
{
    return before->st_dev == after->st_dev &&
           before->st_ino == after->st_ino &&
           before->st_size == after->st_size &&
           before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
           before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
           before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
           before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

static int sync_parent_directory(const char *path)
{
    char *copy = strdup(path);
    if (copy == NULL) return -1;
    char *slash = strrchr(copy, '/');
    const char *parent = ".";
    if (slash != NULL) {
        if (slash == copy) {
            slash[1] = '\0';
            parent = copy;
        } else {
            *slash = '\0';
            parent = copy;
        }
    }
    int directory_fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(copy);
    if (directory_fd < 0) return -1;
    int result = fsync(directory_fd);
    int saved_errno = errno;
    if (close(directory_fd) < 0 && result == 0) {
        return -1;
    }
    errno = saved_errno;
    return result;
}

int copy_regular_file_resumable(const char *source,
                                       const char *destination,
                                       int source_fd,
                                       const struct stat *source_stat,
                                       const Options *options,
                                       Progress *progress,
                                       CopyMetadataFunction set_metadata)
{
    struct stat destination_stat;
    if (lstat(destination, &destination_stat) == 0) {
        if (options->no_clobber) {
            close(source_fd);
            copy_progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
            copy_progress_mark_file(progress);
            return 0;
        }
        if (S_ISLNK(destination_stat.st_mode) ||
            !S_ISREG(destination_stat.st_mode)) {
            fprintf(stderr,
                    "%s: --resume requires a regular-file destination: '%s'\n",
                    program_name, destination);
            close(source_fd);
            return -1;
        }
        if (destination_stat.st_size == source_stat->st_size) {
            int existing_fd = open(destination,
                                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (existing_fd < 0) {
                copy_print_error(destination, "cannot open existing destination");
                close(source_fd);
                return -1;
            }
            struct stat existing_opened_stat;
            if (fstat(existing_fd, &existing_opened_stat) < 0) {
                copy_print_error(destination, "cannot inspect existing destination");
                close(existing_fd);
                close(source_fd);
                return -1;
            }
            if (!S_ISREG(existing_opened_stat.st_mode)) {
                errno = ESTALE;
                copy_print_error(destination, "cannot inspect existing destination");
                close(existing_fd);
                close(source_fd);
                return -1;
            }
            int equal = 0;
            if (existing_opened_stat.st_size == source_stat->st_size) {
                equal = copy_prepare_disk_verification(source_fd, existing_fd,
                                                  source, destination) < 0
                            ? -1
                            : copy_files_equal(source_fd, existing_fd, source,
                                          destination);
            }
            if (equal == 1) {
                struct stat source_after;
                if (fstat(source_fd, &source_after) < 0) {
                    copy_print_error(source, "cannot inspect after verification");
                    equal = -1;
                } else if (!same_source_version(source_stat, &source_after)) {
                    errno = ESTALE;
                    copy_print_error(source, "changed during verification");
                    equal = -1;
                }
            }
            if (equal < 0) {
                close(existing_fd);
                close(source_fd);
                return -1;
            }
            if (equal == 1) {
                int result = 0;
                if (options->preserve &&
                    set_metadata(existing_fd, source_stat, destination,
                                      true) < 0) {
                    result = -1;
                }
                if (result == 0 && options->preserve &&
                    fsync(existing_fd) < 0) {
                    copy_print_error(destination, "cannot sync preserved file");
                    result = -1;
                }
                if (close(existing_fd) < 0 && result == 0) {
                    copy_print_error(destination, "cannot close");
                    result = -1;
                }
                if (close(source_fd) < 0 && result == 0) {
                    copy_print_error(source, "cannot close");
                    result = -1;
                }
                if (result < 0) return -1;
                copy_progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
                copy_progress_mark_file(progress);
                if (options->verbose) {
                    printf("'%s' -> '%s' (already complete)\n",
                           source, destination);
                }
                return 0;
            }
            if (close(existing_fd) < 0) {
                copy_print_error(destination, "cannot close");
                close(source_fd);
                return -1;
            }
        }
    } else if (errno != ENOENT) {
        copy_print_error(destination, "cannot inspect");
        close(source_fd);
        return -1;
    }

    char *partial_path = resume_temporary_path(destination);
    if (partial_path == NULL) {
        copy_print_error(destination, "cannot create resume path for");
        close(source_fd);
        return -1;
    }
    int partial_fd = open(partial_path,
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (partial_fd < 0) {
        copy_print_error(partial_path, "cannot open resume file");
        free(partial_path);
        close(source_fd);
        return -1;
    }
    struct stat partial_stat;
    if (fstat(partial_fd, &partial_stat) < 0) {
        copy_print_error(partial_path, "cannot inspect resume file");
        close(partial_fd);
        free(partial_path);
        close(source_fd);
        return -1;
    }
    if (!S_ISREG(partial_stat.st_mode) || partial_stat.st_uid != geteuid() ||
        partial_stat.st_nlink != 1) {
        errno = EPERM;
        copy_print_error(partial_path, "unsafe resume file");
        close(partial_fd);
        free(partial_path);
        close(source_fd);
        return -1;
    }
    if (flock(partial_fd, LOCK_EX | LOCK_NB) < 0) {
        copy_print_error(partial_path, "resume file is already in use");
        close(partial_fd);
        free(partial_path);
        close(source_fd);
        return -1;
    }

    off_t resume_offset = partial_stat.st_size;
    int prefix_matches = 0;
    if (source_stat->st_size >= 0 && resume_offset >= 0 &&
        resume_offset <= source_stat->st_size) {
        prefix_matches = file_prefix_matches(source_fd, partial_fd,
                                              resume_offset, source,
                                              partial_path);
    }
    if (prefix_matches < 0) {
        close(partial_fd);
        free(partial_path);
        close(source_fd);
        return -1;
    }
    if (prefix_matches == 0) {
        if (ftruncate(partial_fd, 0) < 0) {
            copy_print_error(partial_path, "cannot restart resume file");
            close(partial_fd);
            free(partial_path);
            close(source_fd);
            return -1;
        }
        resume_offset = 0;
    }
    copy_progress_add_bytes(progress, (uintmax_t)resume_offset);

    int result = copy_transfer_file_contents(source_fd, partial_fd, resume_offset,
                                    source_stat->st_size, source, partial_path,
                                    progress);

    if (result == 0 &&
        (copy_prepare_disk_verification(source_fd, partial_fd, source,
                                   partial_path) < 0 ||
         copy_verify_contents(source_fd, partial_fd, source, partial_path) < 0)) {
        result = -1;
    }
    if (result == 0) {
        struct stat source_after;
        if (fstat(source_fd, &source_after) < 0) {
            copy_print_error(source, "cannot inspect after copy");
            result = -1;
        } else if (!same_source_version(source_stat, &source_after)) {
            errno = ESTALE;
            copy_print_error(source, "changed during copy");
            result = -1;
        }
    }
    if (result == 0 && options->preserve &&
        set_metadata(partial_fd, source_stat, partial_path, true) < 0) {
        result = -1;
    }
    if (result == 0 && !options->preserve) {
        mode_t mask = umask(0);
        umask(mask);
        mode_t mode = (source_stat->st_mode & 0777) & ~mask;
        if (fchmod(partial_fd, mode) < 0) {
            copy_print_error(partial_path, "cannot set permissions for");
            result = -1;
        }
    }
    if (result == 0 && fsync(partial_fd) < 0) {
        copy_print_error(partial_path, "cannot sync resume file");
        result = -1;
    }
    if (close(source_fd) < 0 && result == 0) {
        copy_print_error(source, "cannot close");
        result = -1;
    }
    bool published = false;
    if (result == 0) {
        int publish_result;
        if (options->no_clobber) {
            publish_result = link(partial_path, destination);
            if (publish_result == 0) {
                published = true;
                if (unlink(partial_path) < 0) {
                    copy_print_error(partial_path, "cannot remove published resume file");
                    publish_result = -1;
                }
            }
        } else {
            publish_result = rename(partial_path, destination);
            published = publish_result == 0;
        }
        if (publish_result < 0) {
            copy_print_error(destination, "cannot publish completed copy to");
            result = -1;
        } else if (sync_parent_directory(destination) < 0) {
            copy_print_error(destination, "cannot sync destination directory for");
            result = -1;
        }
    }
    if (close(partial_fd) < 0 && result == 0) {
        copy_print_error(partial_path, "cannot close resume file");
        result = -1;
    }
    if (result == 0) {
        copy_progress_mark_file(progress);
        if (options->verbose) printf("'%s' -> '%s'\n", source, destination);
    } else if (!published) {
        fprintf(stderr, "%s: partial data kept for resuming from '%s'\n",
                program_name, partial_path);
    }
    free(partial_path);
    return result;
}
