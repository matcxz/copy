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
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

typedef struct {
    bool recursive;
    bool preserve;
    bool no_clobber;
    bool verbose;
    bool verify;
    bool progress;
} Options;

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
            "  -g, --progress       Show per-file copy progress on the terminal\n"
            "  -h, --help           Show this help\n"
            "  -V, --version        Show version information\n\n"
            "Symbolic links are copied as links. Multiple sources require an\n"
            "existing destination directory.\n",
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

static int verify_contents(int source_fd, int destination_fd,
                           const char *source, const char *destination)
{
    if (lseek(source_fd, 0, SEEK_SET) < 0 ||
        lseek(destination_fd, 0, SEEK_SET) < 0) {
        print_error(source, "cannot seek while verifying");
        return -1;
    }

    unsigned char source_buffer[128 * 1024];
    unsigned char destination_buffer[128 * 1024];
    for (;;) {
        ssize_t source_count = read(source_fd, source_buffer,
                                    sizeof source_buffer);
        if (source_count < 0) {
            if (errno == EINTR) continue;
            print_error(source, "cannot read while verifying");
            return -1;
        }
        ssize_t destination_count = read(destination_fd, destination_buffer,
                                         sizeof destination_buffer);
        if (destination_count < 0) {
            if (errno == EINTR) continue;
            print_error(destination, "cannot read while verifying");
            return -1;
        }
        if (source_count != destination_count ||
            memcmp(source_buffer, destination_buffer,
                   (size_t)source_count) != 0) {
            fprintf(stderr, "%s: verification failed for '%s'\n",
                    program_name, destination);
            errno = EIO;
            return -1;
        }
        if (source_count == 0) {
            return 0;
        }
    }
}

static void show_progress(const char *path, off_t copied, off_t total)
{
    if (total > 0) {
        int percent = (int)(((long double)copied * 100.0L) / total);
        if (percent > 100) percent = 100;
        fprintf(stderr, "\r%s: %d%% (%" PRIuMAX "/%" PRIuMAX " bytes)",
                path, percent, (uintmax_t)copied, (uintmax_t)total);
    } else {
        fprintf(stderr, "\r%s: %" PRIuMAX " bytes", path,
                (uintmax_t)copied);
    }
    fflush(stderr);
}

static int copy_regular_file(const char *source, const char *destination,
                             const struct stat *source_stat,
                             const Options *options)
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
            return 0;
        }
        print_error(destination, "cannot create");
        close(source_fd);
        return -1;
    }

    unsigned char buffer[128 * 1024];
    off_t copied = 0;
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
        copied += count;
        if (options->progress && isatty(STDERR_FILENO)) {
            show_progress(destination, copied, source_stat->st_size);
        }
    }

    if (result == 0 && options->verify &&
        verify_contents(source_fd, destination_fd, source, destination) < 0) {
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
    if (options->progress && isatty(STDERR_FILENO)) {
        fputc('\n', stderr);
    }
    if (result == 0 && options->verbose) {
        printf("'%s' -> '%s'\n", source, destination);
    }
    return result;
}

static int copy_symlink(const char *source, const char *destination,
                        const struct stat *source_stat, const Options *options)
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
    return 0;
}

static int copy_entry(const char *source, const char *destination,
                      const Options *options);

static int copy_directory(const char *source, const char *destination,
                          const struct stat *source_stat,
                          const Options *options)
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
        if (copy_entry(source_child, destination_child, options) < 0) {
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
                      const Options *options)
{
    struct stat source_stat;
    if (lstat(source, &source_stat) < 0) {
        print_error(source, "cannot access");
        return -1;
    }

    if (S_ISLNK(source_stat.st_mode)) {
        return copy_symlink(source, destination, &source_stat, options);
    }
    if (S_ISDIR(source_stat.st_mode)) {
        return copy_directory(source, destination, &source_stat, options);
    }
    if (S_ISREG(source_stat.st_mode)) {
        return copy_regular_file(source, destination, &source_stat, options);
    }

    fprintf(stderr, "%s: unsupported file type: '%s'\n", program_name, source);
    return -1;
}

static bool path_is_within(const char *parent, const char *child)
{
    size_t length = strlen(parent);
    while (length > 1 && parent[length - 1] == '/') length--;
    if (length == 1 && parent[0] == '/') {
        return child[0] == '/';
    }
    return strncmp(parent, child, length) == 0 &&
           (child[length] == '/' || child[length] == '\0');
}

static char *canonical_destination(const char *path)
{
    char *copy = strdup(path);
    if (copy == NULL) return NULL;

    char *slash = strrchr(copy, '/');
    const char *parent_path = ".";
    char *base = copy;
    if (slash != NULL) {
        *slash = '\0';
        parent_path = copy[0] == '\0' ? "/" : copy;
        base = slash + 1;
    }
    if (*base == '\0') {
        free(copy);
        return realpath(path, NULL);
    }

    char *canonical_parent = realpath(parent_path, NULL);
    if (canonical_parent == NULL) {
        free(copy);
        return NULL;
    }
    char *canonical = join_path(canonical_parent, base);
    free(canonical_parent);
    free(copy);
    return canonical;
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

        struct stat source_stat;
        if (lstat(source, &source_stat) == 0 && S_ISDIR(source_stat.st_mode)) {
            char *canonical_source = realpath(source, NULL);
            char *canonical_target = canonical_destination(target);
            if (canonical_source != NULL && canonical_target != NULL &&
                path_is_within(canonical_source, canonical_target)) {
                fprintf(stderr,
                        "%s: cannot copy directory '%s' into itself ('%s')\n",
                        program_name, source, target);
                failures++;
                free(canonical_source);
                free(canonical_target);
                free(target);
                continue;
            }
            free(canonical_source);
            free(canonical_target);
        }

        if (copy_entry(source, target, options) < 0) {
            failures++;
        }
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
