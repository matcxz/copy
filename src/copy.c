#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    bool recursive;
    bool preserve;
    bool no_clobber;
    bool verbose;
    bool verify;
    bool progress;
    bool resume;
} Options;

typedef struct {
    bool enabled;
    uintmax_t total_bytes;
    uintmax_t processed_bytes;
    size_t total_files;
    size_t processed_files;
    int last_percent;
} Progress;

static const char *program_name = "copy";

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "copy — An independent, lightweight alternative to cp.\n\n"
            "Usage: %s [OPTIONS] SOURCE... DESTINATION\n\n"
            "Options:\n"
            "  -R, -r, --recursive  Copy directories and their contents\n"
            "  -p, --preserve       Preserve file modes and modification times\n"
            "  -n, --no-clobber     Never overwrite an existing destination\n"
            "  -v, --verbose        Print each copied path\n"
            "  -c, --verify         Compare copied file contents with the source\n"
            "  -g, --progress       Show overall copy progress on the terminal\n"
            "      --resume         Resume regular-file copies; verify before publishing\n"
            "  -h, --help           Show this help\n"
            "  -V, --version        Show version information\n\n"
            "Symbolic links are copied as links. Multiple sources require an\n"
            "existing destination directory. Resume data is kept in a hidden\n"
            "file beside the destination until the copy is verified.\n",
            program_name);
}

static void print_error(const char *path, const char *action)
{
    fprintf(stderr, "%s: %s '%s': %s\n", program_name, action, path,
            strerror(errno));
}

static char *join_path(const char *left, const char *right)
{
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    bool separator_needed = left_len > 0 && left[left_len - 1] != '/';

    if (left_len > SIZE_MAX - right_len - (separator_needed ? 2 : 1)) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    size_t length = left_len + right_len + (separator_needed ? 1 : 0) + 1;
    char *joined = malloc(length);
    if (joined == NULL) {
        return NULL;
    }
    snprintf(joined, length, "%s%s%s", left,
             separator_needed ? "/" : "", right);
    return joined;
}

static char *path_basename(const char *path)
{
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') {
        length--;
    }
    size_t start = length;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }
    if (start == length) {
        return strdup("/");
    }
    size_t name_length = length - start;
    char *name = malloc(name_length + 1);
    if (name == NULL) {
        return NULL;
    }
    memcpy(name, path + start, name_length);
    name[name_length] = '\0';
    return name;
}

static bool same_file(const struct stat *source, const struct stat *destination)
{
    return source->st_dev == destination->st_dev &&
           source->st_ino == destination->st_ino;
}

static int set_file_metadata(int fd, const struct stat *source,
                             const char *path, bool preserve)
{
    if (!preserve) {
        return 0;
    }
    if (fchmod(fd, source->st_mode & 07777) < 0) {
        print_error(path, "cannot preserve permissions for");
        return -1;
    }
    struct timespec times[2] = {source->st_atim, source->st_mtim};
    if (futimens(fd, times) < 0) {
        print_error(path, "cannot preserve timestamps for");
        return -1;
    }
    return 0;
}

static ssize_t read_block_at(int fd, unsigned char *buffer, size_t capacity,
                             off_t offset, const char *path,
                             const char *action)
{
    size_t total = 0;
    while (total < capacity) {
        ssize_t count = pread(fd, buffer + total, capacity - total,
                              offset + (off_t)total);
        if (count < 0) {
            if (errno == EINTR) continue;
            print_error(path, action);
            return -1;
        }
        if (count == 0) break;
        total += (size_t)count;
    }
    return (ssize_t)total;
}

static int files_equal(int source_fd, int destination_fd,
                       const char *source, const char *destination)
{
    unsigned char source_buffer[128 * 1024];
    unsigned char destination_buffer[128 * 1024];
    off_t offset = 0;
    for (;;) {
        ssize_t source_count = read_block_at(source_fd, source_buffer,
                                             sizeof source_buffer, offset,
                                             source, "cannot read while verifying");
        if (source_count < 0) return -1;
        ssize_t destination_count = read_block_at(
            destination_fd, destination_buffer, sizeof destination_buffer,
            offset, destination, "cannot read while verifying");
        if (destination_count < 0) return -1;
        if (source_count != destination_count ||
            memcmp(source_buffer, destination_buffer,
                   (size_t)source_count) != 0) {
            return 0;
        }
        if (source_count == 0) {
            return 1;
        }
        offset += source_count;
    }
}

static int verify_contents(int source_fd, int destination_fd,
                           const char *source, const char *destination)
{
    int equal = files_equal(source_fd, destination_fd, source, destination);
    if (equal == 0) {
        fprintf(stderr, "%s: verification failed for '%s'\n",
                program_name, destination);
        errno = EIO;
        return -1;
    }
    return equal;
}

static int prepare_disk_verification(int source_fd, int destination_fd,
                                     const char *source,
                                     const char *destination)
{
    if (fsync(destination_fd) < 0) {
        print_error(destination, "cannot sync before verification");
        return -1;
    }
    int advice_result = posix_fadvise(destination_fd, 0, 0,
                                      POSIX_FADV_DONTNEED);
    if (advice_result != 0) {
        errno = advice_result;
        print_error(destination, "cannot discard cached data for");
        return -1;
    }
    advice_result = posix_fadvise(source_fd, 0, 0, POSIX_FADV_DONTNEED);
    if (advice_result != 0) {
        errno = advice_result;
        print_error(source, "cannot discard cached data for");
        return -1;
    }
    return 0;
}

static void show_progress(Progress *progress)
{
    int percent;
    if (progress->total_bytes > 0) {
        long double ratio = ((long double)progress->processed_bytes * 100.0L) /
                            progress->total_bytes;
        percent = ratio >= 100.0L ? 100 : (int)ratio;
    } else {
        long double ratio = progress->total_files > 0
                                ? ((long double)progress->processed_files * 100.0L) /
                                      progress->total_files
                                : 100.0L;
        percent = ratio >= 100.0L ? 100 : (int)ratio;
    }
    if (percent == progress->last_percent) return;
    progress->last_percent = percent;
    if (progress->total_bytes > 0) {
        fprintf(stderr, "\rOverall: %d%% (%" PRIuMAX "/%" PRIuMAX " bytes)",
                percent, progress->processed_bytes, progress->total_bytes);
    } else {
        fprintf(stderr, "\rOverall: %d%% (%zu/%zu files)",
                percent, progress->processed_files, progress->total_files);
    }
    fflush(stderr);
}

static void progress_add_bytes(Progress *progress, uintmax_t amount)
{
    if (progress == NULL || !progress->enabled) return;
    if (amount >= progress->total_bytes - progress->processed_bytes) {
        progress->processed_bytes = progress->total_bytes;
    } else {
        progress->processed_bytes += amount;
    }
    show_progress(progress);
}

static void progress_mark_file(Progress *progress)
{
    if (progress == NULL || !progress->enabled) return;
    if (progress->processed_files < progress->total_files) {
        progress->processed_files++;
    }
    if (progress->total_bytes == 0) {
        show_progress(progress);
    }
}

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
    char *result = join_path(parent, temporary_name);
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
        ssize_t source_count = read_block_at(source_fd, source_buffer, requested,
                                             offset, source,
                                             "cannot read while resuming");
        if (source_count < 0) return -1;
        ssize_t partial_count = read_block_at(
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

static int copy_regular_file_resumable(const char *source,
                                       const char *destination,
                                       int source_fd,
                                       const struct stat *source_stat,
                                       const Options *options,
                                       Progress *progress)
{
    struct stat destination_stat;
    if (lstat(destination, &destination_stat) == 0) {
        if (options->no_clobber) {
            close(source_fd);
            progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
            progress_mark_file(progress);
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
                print_error(destination, "cannot open existing destination");
                close(source_fd);
                return -1;
            }
            struct stat existing_opened_stat;
            if (fstat(existing_fd, &existing_opened_stat) < 0) {
                print_error(destination, "cannot inspect existing destination");
                close(existing_fd);
                close(source_fd);
                return -1;
            }
            if (!S_ISREG(existing_opened_stat.st_mode)) {
                errno = ESTALE;
                print_error(destination, "cannot inspect existing destination");
                close(existing_fd);
                close(source_fd);
                return -1;
            }
            int equal = 0;
            if (existing_opened_stat.st_size == source_stat->st_size) {
                equal = prepare_disk_verification(source_fd, existing_fd,
                                                  source, destination) < 0
                            ? -1
                            : files_equal(source_fd, existing_fd, source,
                                          destination);
            }
            if (equal == 1) {
                struct stat source_after;
                if (fstat(source_fd, &source_after) < 0) {
                    print_error(source, "cannot inspect after verification");
                    equal = -1;
                } else if (!same_source_version(source_stat, &source_after)) {
                    errno = ESTALE;
                    print_error(source, "changed during verification");
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
                    set_file_metadata(existing_fd, source_stat, destination,
                                      true) < 0) {
                    result = -1;
                }
                if (result == 0 && options->preserve &&
                    fsync(existing_fd) < 0) {
                    print_error(destination, "cannot sync preserved file");
                    result = -1;
                }
                if (close(existing_fd) < 0 && result == 0) {
                    print_error(destination, "cannot close");
                    result = -1;
                }
                if (close(source_fd) < 0 && result == 0) {
                    print_error(source, "cannot close");
                    result = -1;
                }
                if (result < 0) return -1;
                progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
                progress_mark_file(progress);
                if (options->verbose) {
                    printf("'%s' -> '%s' (already complete)\n",
                           source, destination);
                }
                return 0;
            }
            if (close(existing_fd) < 0) {
                print_error(destination, "cannot close");
                close(source_fd);
                return -1;
            }
        }
    } else if (errno != ENOENT) {
        print_error(destination, "cannot inspect");
        close(source_fd);
        return -1;
    }

    char *partial_path = resume_temporary_path(destination);
    if (partial_path == NULL) {
        print_error(destination, "cannot create resume path for");
        close(source_fd);
        return -1;
    }
    int partial_fd = open(partial_path,
                          O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (partial_fd < 0) {
        print_error(partial_path, "cannot open resume file");
        free(partial_path);
        close(source_fd);
        return -1;
    }
    struct stat partial_stat;
    if (fstat(partial_fd, &partial_stat) < 0) {
        print_error(partial_path, "cannot inspect resume file");
        close(partial_fd);
        free(partial_path);
        close(source_fd);
        return -1;
    }
    if (!S_ISREG(partial_stat.st_mode) || partial_stat.st_uid != geteuid() ||
        partial_stat.st_nlink != 1) {
        errno = EPERM;
        print_error(partial_path, "unsafe resume file");
        close(partial_fd);
        free(partial_path);
        close(source_fd);
        return -1;
    }
    if (flock(partial_fd, LOCK_EX | LOCK_NB) < 0) {
        print_error(partial_path, "resume file is already in use");
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
            print_error(partial_path, "cannot restart resume file");
            close(partial_fd);
            free(partial_path);
            close(source_fd);
            return -1;
        }
        resume_offset = 0;
    }
    if (lseek(source_fd, resume_offset, SEEK_SET) < 0 ||
        lseek(partial_fd, resume_offset, SEEK_SET) < 0) {
        print_error(partial_path, "cannot seek resume file");
        close(partial_fd);
        free(partial_path);
        close(source_fd);
        return -1;
    }
    progress_add_bytes(progress, (uintmax_t)resume_offset);

    unsigned char buffer[128 * 1024];
    int result = 0;
    for (;;) {
        ssize_t count = read(source_fd, buffer, sizeof buffer);
        if (count < 0) {
            if (errno == EINTR) continue;
            print_error(source, "cannot read");
            result = -1;
            break;
        }
        if (count == 0) break;

        ssize_t offset = 0;
        while (offset < count) {
            ssize_t written = write(partial_fd, buffer + offset,
                                    (size_t)(count - offset));
            if (written < 0) {
                if (errno == EINTR) continue;
                print_error(partial_path, "cannot write resume file");
                result = -1;
                break;
            }
            if (written == 0) {
                errno = EIO;
                print_error(partial_path, "cannot write resume file");
                result = -1;
                break;
            }
            offset += written;
        }
        if (result < 0) break;
        progress_add_bytes(progress, (uintmax_t)count);
    }

    if (result == 0 &&
        (prepare_disk_verification(source_fd, partial_fd, source,
                                   partial_path) < 0 ||
         verify_contents(source_fd, partial_fd, source, partial_path) < 0)) {
        result = -1;
    }
    if (result == 0) {
        struct stat source_after;
        if (fstat(source_fd, &source_after) < 0) {
            print_error(source, "cannot inspect after copy");
            result = -1;
        } else if (!same_source_version(source_stat, &source_after)) {
            errno = ESTALE;
            print_error(source, "changed during copy");
            result = -1;
        }
    }
    if (result == 0 && options->preserve &&
        set_file_metadata(partial_fd, source_stat, partial_path, true) < 0) {
        result = -1;
    }
    if (result == 0 && !options->preserve) {
        mode_t mask = umask(0);
        umask(mask);
        if (fchmod(partial_fd, 0666 & ~mask) < 0) {
            print_error(partial_path, "cannot set permissions for");
            result = -1;
        }
    }
    if (result == 0 && fsync(partial_fd) < 0) {
        print_error(partial_path, "cannot sync resume file");
        result = -1;
    }
    if (close(source_fd) < 0 && result == 0) {
        print_error(source, "cannot close");
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
                    print_error(partial_path, "cannot remove published resume file");
                    publish_result = -1;
                }
            }
        } else {
            publish_result = rename(partial_path, destination);
            published = publish_result == 0;
        }
        if (publish_result < 0) {
            print_error(destination, "cannot publish completed copy to");
            result = -1;
        } else if (sync_parent_directory(destination) < 0) {
            print_error(destination, "cannot sync destination directory for");
            result = -1;
        }
    }
    if (close(partial_fd) < 0 && result == 0) {
        print_error(partial_path, "cannot close resume file");
        result = -1;
    }
    if (result == 0) {
        progress_mark_file(progress);
        if (options->verbose) printf("'%s' -> '%s'\n", source, destination);
    } else if (!published) {
        fprintf(stderr, "%s: partial data kept for resuming from '%s'\n",
                program_name, partial_path);
    }
    free(partial_path);
    return result;
}

static int copy_regular_file(const char *source, const char *destination,
                             const struct stat *source_stat,
                             const Options *options, Progress *progress)
{
    int source_fd = open(source, O_RDONLY | O_NOFOLLOW);
    if (source_fd < 0) {
        print_error(source, "cannot open");
        return -1;
    }

    struct stat opened_stat;
    if (fstat(source_fd, &opened_stat) < 0) {
        print_error(source, "cannot inspect");
        close(source_fd);
        return -1;
    }
    if (!S_ISREG(opened_stat.st_mode)) {
        errno = EINVAL;
        print_error(source, "not a regular file");
        close(source_fd);
        return -1;
    }

    struct stat destination_stat;
    if (stat(destination, &destination_stat) == 0) {
        if (same_file(&opened_stat, &destination_stat)) {
            fprintf(stderr, "%s: '%s' and '%s' are the same file\n",
                    program_name, source, destination);
            close(source_fd);
            return -1;
        }
    } else if (errno != ENOENT) {
        print_error(destination, "cannot inspect");
        close(source_fd);
        return -1;
    }

    if (options->resume) {
        return copy_regular_file_resumable(source, destination, source_fd,
                                           &opened_stat, options, progress);
    }

    int flags = (options->verify ? O_RDWR : O_WRONLY) | O_CREAT | O_CLOEXEC;
    if (options->no_clobber) {
        flags |= O_EXCL;
    } else {
        flags |= O_TRUNC;
    }
    int destination_fd = open(destination, flags, 0666);
    if (destination_fd < 0) {
        if (options->no_clobber && errno == EEXIST) {
            close(source_fd);
            progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
            progress_mark_file(progress);
            return 0;
        }
        print_error(destination, "cannot create");
        close(source_fd);
        return -1;
    }

    unsigned char buffer[128 * 1024];
    uintmax_t copied = 0;
    int result = 0;
    for (;;) {
        ssize_t count = read(source_fd, buffer, sizeof buffer);
        if (count < 0) {
            if (errno == EINTR) continue;
            print_error(source, "cannot read");
            result = -1;
            break;
        }
        if (count == 0) break;

        ssize_t offset = 0;
        while (offset < count) {
            ssize_t written = write(destination_fd, buffer + offset,
                                    (size_t)(count - offset));
            if (written < 0) {
                if (errno == EINTR) continue;
                print_error(destination, "cannot write");
                result = -1;
                break;
            }
            offset += written;
        }
        if (result < 0) break;
        if (progress != NULL && progress->enabled) {
            progress_add_bytes(progress, (uintmax_t)count);
            if (UINTMAX_MAX - copied < (uintmax_t)count) {
                copied = UINTMAX_MAX;
            } else {
                copied += (uintmax_t)count;
            }
        }
    }

    if (result == 0 && options->verify &&
        (prepare_disk_verification(source_fd, destination_fd, source,
                                   destination) < 0 ||
         verify_contents(source_fd, destination_fd, source, destination) < 0)) {
        result = -1;
    }
    if (result == 0 && set_file_metadata(destination_fd, source_stat,
                                         destination, options->preserve) < 0) {
        result = -1;
    }
    if (close(destination_fd) < 0 && result == 0) {
        print_error(destination, "cannot close");
        result = -1;
    }
    if (close(source_fd) < 0 && result == 0) {
        print_error(source, "cannot close");
        result = -1;
    }
    if (result == 0) {
        uintmax_t expected = source_stat->st_size > 0
                                 ? (uintmax_t)source_stat->st_size
                                 : 0;
        if (copied < expected) progress_add_bytes(progress, expected - copied);
        progress_mark_file(progress);
    }
    if (result == 0 && options->verbose) {
        printf("'%s' -> '%s'\n", source, destination);
    }
    return result;
}

static int copy_symlink(const char *source, const char *destination,
                        const struct stat *source_stat, const Options *options,
                        Progress *progress)
{
    size_t capacity = source_stat->st_size > 0
                          ? (size_t)source_stat->st_size + 1
                          : 256;
    char *target = NULL;
    for (;;) {
        target = malloc(capacity + 1);
        if (target == NULL) {
            print_error(source, "cannot allocate symbolic link target for");
            return -1;
        }
        ssize_t length = readlink(source, target, capacity);
        if (length < 0) {
            print_error(source, "cannot read symbolic link");
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
            print_error(source, "symbolic link target is too long for");
            return -1;
        }
        capacity *= 2;
    }

    struct stat existing;
    if (lstat(destination, &existing) == 0) {
        if (options->no_clobber) {
            free(target);
            progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
            progress_mark_file(progress);
            return 0;
        }
        if (S_ISDIR(existing.st_mode) && !S_ISLNK(existing.st_mode)) {
            fprintf(stderr, "%s: refusing to replace directory '%s' with a link\n",
                    program_name, destination);
            free(target);
            return -1;
        }
        if (unlink(destination) < 0) {
            print_error(destination, "cannot replace");
            free(target);
            return -1;
        }
    } else if (errno != ENOENT) {
        print_error(destination, "cannot inspect");
        free(target);
        return -1;
    }

    if (symlink(target, destination) < 0) {
        print_error(destination, "cannot create symbolic link");
        free(target);
        return -1;
    }
    free(target);

    if (options->preserve) {
        struct timespec times[2] = {source_stat->st_atim, source_stat->st_mtim};
        if (utimensat(AT_FDCWD, destination, times, AT_SYMLINK_NOFOLLOW) < 0) {
            print_error(destination, "cannot preserve symbolic link timestamps for");
            return -1;
        }
    }
    if (options->verbose) {
        printf("'%s' -> '%s' (symbolic link)\n", source, destination);
    }
    progress_add_bytes(progress, (uintmax_t)source_stat->st_size);
    progress_mark_file(progress);
    return 0;
}

static int copy_entry(const char *source, const char *destination,
                      const Options *options, Progress *progress);

static int check_directory_destination(const char *source,
                                       const char *destination,
                                       const struct stat *source_stat)
{
    char *candidate = strdup(destination);
    if (candidate == NULL) {
        print_error(destination, "cannot allocate path for");
        return -1;
    }

    int directory_fd = -1;
    for (;;) {
        directory_fd = open(candidate, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd >= 0) break;
        if (errno != ENOENT) {
            print_error(candidate, "cannot inspect destination ancestor");
            free(candidate);
            return -1;
        }

        size_t length = strlen(candidate);
        while (length > 1 && candidate[length - 1] == '/') {
            candidate[--length] = '\0';
        }
        char *slash = strrchr(candidate, '/');
        if (slash == NULL) {
            strcpy(candidate, ".");
        } else if (slash == candidate) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
        if (strcmp(candidate, ".") == 0) {
            directory_fd = open(candidate, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (directory_fd < 0) {
                print_error(candidate, "cannot inspect destination ancestor");
                free(candidate);
                return -1;
            }
            break;
        }
    }
    free(candidate);

    for (;;) {
        struct stat current_stat;
        if (fstat(directory_fd, &current_stat) < 0) {
            print_error(destination, "cannot inspect destination ancestor");
            close(directory_fd);
            return -1;
        }
        if (current_stat.st_dev == source_stat->st_dev &&
            current_stat.st_ino == source_stat->st_ino) {
            fprintf(stderr,
                    "%s: cannot copy directory '%s' into itself ('%s')\n",
                    program_name, source, destination);
            close(directory_fd);
            return -1;
        }

        int parent_fd = openat(directory_fd, "..",
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent_fd < 0) {
            print_error(destination, "cannot inspect destination ancestor");
            close(directory_fd);
            return -1;
        }
        struct stat parent_stat;
        if (fstat(parent_fd, &parent_stat) < 0) {
            print_error(destination, "cannot inspect destination ancestor");
            close(parent_fd);
            close(directory_fd);
            return -1;
        }
        if (parent_stat.st_dev == current_stat.st_dev &&
            parent_stat.st_ino == current_stat.st_ino) {
            close(parent_fd);
            close(directory_fd);
            return 0;
        }
        close(directory_fd);
        directory_fd = parent_fd;
    }
}

static int copy_directory(const char *source, const char *destination,
                          const struct stat *source_stat,
                          const Options *options, Progress *progress)
{
    if (!options->recursive) {
        fprintf(stderr, "%s: omitting directory '%s' (use --recursive)\n",
                program_name, source);
        return -1;
    }

    struct stat destination_stat;
    bool destination_exists = lstat(destination, &destination_stat) == 0;
    if (destination_exists && !S_ISDIR(destination_stat.st_mode)) {
        fprintf(stderr, "%s: destination '%s' is not a directory\n",
                program_name, destination);
        return -1;
    }
    if (!destination_exists && errno != ENOENT) {
        print_error(destination, "cannot inspect");
        return -1;
    }
    if (check_directory_destination(source, destination, source_stat) < 0) {
        return -1;
    }

    if (!destination_exists && mkdir(destination, 0777) < 0) {
        print_error(destination, "cannot create directory");
        return -1;
    }

    DIR *directory = opendir(source);
    if (directory == NULL) {
        print_error(source, "cannot open directory");
        return -1;
    }

    int result = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                print_error(source, "cannot read directory");
                result = -1;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *source_child = join_path(source, entry->d_name);
        char *destination_child = join_path(destination, entry->d_name);
        if (source_child == NULL || destination_child == NULL) {
            print_error(source, "cannot allocate path for");
            free(source_child);
            free(destination_child);
            result = -1;
            continue;
        }
        if (copy_entry(source_child, destination_child, options, progress) < 0) {
            result = -1;
        }
        free(source_child);
        free(destination_child);
    }
    if (closedir(directory) < 0) {
        print_error(source, "cannot close directory");
        result = -1;
    }

    if (options->preserve) {
        if (chmod(destination, source_stat->st_mode & 07777) < 0) {
            print_error(destination, "cannot preserve permissions for");
            result = -1;
        } else {
            struct timespec times[2] = {source_stat->st_atim,
                                         source_stat->st_mtim};
            if (utimensat(AT_FDCWD, destination, times, 0) < 0) {
                print_error(destination, "cannot preserve timestamps for");
                result = -1;
            }
        }
    }
    if (result == 0 && options->verbose) {
        printf("'%s' -> '%s'\n", source, destination);
    }
    return result;
}

static int copy_entry(const char *source, const char *destination,
                      const Options *options, Progress *progress)
{
    struct stat source_stat;
    if (lstat(source, &source_stat) < 0) {
        print_error(source, "cannot access");
        return -1;
    }

    if (S_ISLNK(source_stat.st_mode)) {
        return copy_symlink(source, destination, &source_stat, options,
                            progress);
    }
    if (S_ISDIR(source_stat.st_mode)) {
        return copy_directory(source, destination, &source_stat, options,
                              progress);
    }
    if (S_ISREG(source_stat.st_mode)) {
        return copy_regular_file(source, destination, &source_stat, options,
                                 progress);
    }

    fprintf(stderr, "%s: unsupported file type: '%s'\n", program_name, source);
    return -1;
}

static int add_progress_file(Progress *progress, const struct stat *file_stat,
                             const char *path)
{
    if (file_stat->st_size < 0) {
        errno = EOVERFLOW;
        print_error(path, "cannot determine size of");
        return -1;
    }
    uintmax_t size = (uintmax_t)file_stat->st_size;
    if (UINTMAX_MAX - progress->total_bytes < size ||
        progress->total_files == SIZE_MAX) {
        errno = EOVERFLOW;
        print_error(path, "too many bytes or files to track for");
        return -1;
    }
    progress->total_bytes += size;
    progress->total_files++;
    return 0;
}

static int scan_progress_tree(const char *path, bool recursive,
                              Progress *progress)
{
    struct stat file_stat;
    if (lstat(path, &file_stat) < 0) {
        print_error(path, "cannot access while calculating progress for");
        return -1;
    }
    if (S_ISREG(file_stat.st_mode) || S_ISLNK(file_stat.st_mode)) {
        return add_progress_file(progress, &file_stat, path);
    }
    if (!S_ISDIR(file_stat.st_mode) || !recursive) {
        return 0;
    }

    DIR *directory = opendir(path);
    if (directory == NULL) {
        print_error(path, "cannot open while calculating progress for");
        return -1;
    }

    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                print_error(path, "cannot read while calculating progress for");
                result = -1;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *child = join_path(path, entry->d_name);
        if (child == NULL) {
            print_error(path, "cannot allocate progress path for");
            result = -1;
            continue;
        }
        if (scan_progress_tree(child, recursive, progress) < 0) {
            result = -1;
        }
        free(child);
    }
    if (closedir(directory) < 0) {
        print_error(path, "cannot close while calculating progress for");
        result = -1;
    }
    return result;
}

static int main_copy(int argc, char **argv, const Options *options)
{
    int operand_count = argc;
    if (operand_count < 2) {
        print_usage(stderr);
        return 2;
    }

    const char *destination = argv[operand_count - 1];
    int source_count = operand_count - 1;
    struct stat destination_stat;
    bool destination_is_directory =
        stat(destination, &destination_stat) == 0 &&
        S_ISDIR(destination_stat.st_mode);
    if (source_count > 1 && !destination_is_directory) {
        fprintf(stderr,
                "%s: destination '%s' must be an existing directory when copying multiple sources\n",
                program_name, destination);
        return 2;
    }

    int failures = 0;
    for (int index = 0; index < source_count; index++) {
        const char *source = argv[index];
        char *target = NULL;
        if (destination_is_directory) {
            char *base = path_basename(source);
            if (base == NULL) {
                print_error(source, "cannot allocate destination name for");
                failures++;
                continue;
            }
            target = join_path(destination, base);
            free(base);
        } else {
            target = strdup(destination);
        }
        if (target == NULL) {
            print_error(destination, "cannot allocate destination path for");
            failures++;
            continue;
        }

        Progress progress = {
            .enabled = options->progress && isatty(STDERR_FILENO),
            .last_percent = -1
        };
        if (progress.enabled &&
            scan_progress_tree(source, options->recursive, &progress) < 0) {
            failures++;
            free(target);
            continue;
        }
        if (copy_entry(source, target, options,
                       progress.enabled ? &progress : NULL) < 0) {
            failures++;
        }
        if (progress.last_percent >= 0) fputc('\n', stderr);
        free(target);
    }
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    Options options = {0};
    char **operands = calloc((size_t)argc, sizeof *operands);
    if (operands == NULL) {
        perror(program_name);
        return 1;
    }
    int operand_count = 0;
    bool parse_options = true;

    for (int index = 1; index < argc; index++) {
        const char *arg = argv[index];
        if (parse_options && strcmp(arg, "--") == 0) {
            parse_options = false;
            continue;
        }
        if (parse_options && arg[0] == '-' && arg[1] != '-' &&
            arg[1] != '\0' && arg[2] != '\0') {
            for (const char *option = arg + 1; *option != '\0'; option++) {
                switch (*option) {
                case 'r':
                case 'R':
                    options.recursive = true;
                    break;
                case 'p':
                    options.preserve = true;
                    break;
                case 'n':
                    options.no_clobber = true;
                    break;
                case 'v':
                    options.verbose = true;
                    break;
                case 'c':
                    options.verify = true;
                    break;
                case 'g':
                    options.progress = true;
                    break;
                case 'h':
                    print_usage(stdout);
                    free(operands);
                    return 0;
                case 'V':
                    puts("copy 0.1.0");
                    free(operands);
                    return 0;
                default:
                    fprintf(stderr, "%s: unknown option: '-%c'\n",
                            program_name, *option);
                    print_usage(stderr);
                    free(operands);
                    return 2;
                }
            }
            continue;
        }
        if (parse_options && arg[0] == '-' && arg[1] != '\0') {
            if (strcmp(arg, "-R") == 0 || strcmp(arg, "-r") == 0 ||
                strcmp(arg, "--recursive") == 0) {
                options.recursive = true;
            } else if (strcmp(arg, "-p") == 0 ||
                       strcmp(arg, "--preserve") == 0) {
                options.preserve = true;
            } else if (strcmp(arg, "-n") == 0 ||
                       strcmp(arg, "--no-clobber") == 0) {
                options.no_clobber = true;
            } else if (strcmp(arg, "-v") == 0 ||
                       strcmp(arg, "--verbose") == 0) {
                options.verbose = true;
            } else if (strcmp(arg, "-c") == 0 ||
                       strcmp(arg, "--verify") == 0) {
                options.verify = true;
            } else if (strcmp(arg, "-g") == 0 ||
                       strcmp(arg, "--progress") == 0) {
                options.progress = true;
            } else if (strcmp(arg, "--resume") == 0) {
                options.resume = true;
            } else if (strcmp(arg, "-h") == 0 ||
                       strcmp(arg, "--help") == 0) {
                print_usage(stdout);
                free(operands);
                return 0;
            } else if (strcmp(arg, "-V") == 0 ||
                       strcmp(arg, "--version") == 0) {
                puts("copy 0.1.0");
                free(operands);
                return 0;
            } else {
                fprintf(stderr, "%s: unknown option: '%s'\n",
                        program_name, arg);
                print_usage(stderr);
                free(operands);
                return 2;
            }
        } else {
            operands[operand_count++] = argv[index];
        }
    }

    if (operand_count < 2) {
        print_usage(stderr);
        free(operands);
        return 2;
    }
    int result = main_copy(operand_count, operands, &options);
    free(operands);
    return result;
}
