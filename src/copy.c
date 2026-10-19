#include "common.h"

#include "options.h"
#include "progress.h"
#include "resume.h"
#include "transfer.h"
#include "util.h"
#include "verify.h"

#include "progress.c"
#include "resume.c"
#include "transfer.c"
#include "util.c"
#include "verify.c"

const char *program_name = "copy";

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
            "      --dry-run        Show planned changes without copying anything\n"
            "  -h, --help           Show this help\n"
            "  -V, --version        Show version information\n\n"
            "Symbolic links are copied as links. Multiple sources require an\n"
            "existing destination directory. Resume data is kept in a hidden\n"
            "file beside the destination until the copy is verified.\n",
            program_name);
}

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

static int copy_regular_file(const char *source, const char *destination,
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

static int copy_symlink(const char *source, const char *destination,
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

static int copy_entry(const char *source, const char *destination,
                      const Options *options, Progress *progress);

static int check_directory_destination(const char *source,
                                       const char *destination,
                                       const struct stat *source_stat)
{
    char *candidate = strdup(destination);
    if (candidate == NULL) {
        copy_print_error(destination, "cannot allocate path for");
        return -1;
    }

    int directory_fd = -1;
    for (;;) {
        directory_fd = open(candidate, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory_fd >= 0) break;
        if (errno != ENOENT) {
            copy_print_error(candidate, "cannot inspect destination ancestor");
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
                copy_print_error(candidate, "cannot inspect destination ancestor");
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
            copy_print_error(destination, "cannot inspect destination ancestor");
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
            copy_print_error(destination, "cannot inspect destination ancestor");
            close(directory_fd);
            return -1;
        }
        struct stat parent_stat;
        if (fstat(parent_fd, &parent_stat) < 0) {
            copy_print_error(destination, "cannot inspect destination ancestor");
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
        copy_print_error(destination, "cannot inspect");
        return -1;
    }
    if (check_directory_destination(source, destination, source_stat) < 0) {
        return -1;
    }

    if (options->dry_run) {
        copy_print_dry_run(destination_exists ? "merge directory" : "create directory",
                      source, destination);
    }
    if (!destination_exists && !options->dry_run &&
        mkdir(destination, (source_stat->st_mode & 0777) | 0700) < 0) {
        copy_print_error(destination, "cannot create directory");
        return -1;
    }

    DIR *directory = opendir(source);
    if (directory == NULL) {
        copy_print_error(source, "cannot open directory");
        return -1;
    }

    int result = 0;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                copy_print_error(source, "cannot read directory");
                result = -1;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *source_child = copy_join_path(source, entry->d_name);
        char *destination_child = copy_join_path(destination, entry->d_name);
        if (source_child == NULL || destination_child == NULL) {
            copy_print_error(source, "cannot allocate path for");
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
        copy_print_error(source, "cannot close directory");
        result = -1;
    }

    if (options->preserve && !options->dry_run) {
        if (chmod(destination, source_stat->st_mode & 07777) < 0) {
            copy_print_error(destination, "cannot preserve permissions for");
            result = -1;
        } else {
            struct timespec times[2] = {source_stat->st_atim,
                                         source_stat->st_mtim};
            if (utimensat(AT_FDCWD, destination, times, 0) < 0) {
                copy_print_error(destination, "cannot preserve timestamps for");
                result = -1;
            }
        }
    } else if (!options->dry_run && !destination_exists) {
        mode_t mask = umask(0);
        umask(mask);
        mode_t mode = (source_stat->st_mode & 0777) & ~mask;
        if (chmod(destination, mode) < 0) {
            copy_print_error(destination, "cannot set permissions for");
            result = -1;
        }
    }
    if (result == 0 && options->verbose && !options->dry_run) {
        printf("'%s' -> '%s'\n", source, destination);
    }
    return result;
}

static int copy_entry(const char *source, const char *destination,
                      const Options *options, Progress *progress)
{
    struct stat source_stat;
    if (lstat(source, &source_stat) < 0) {
        copy_print_error(source, "cannot access");
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
    char **seen_targets = calloc((size_t)source_count, sizeof *seen_targets);
    if (seen_targets == NULL) {
        perror(program_name);
        return 1;
    }
    int seen_count = 0;
    for (int index = 0; index < source_count; index++) {
        const char *source = argv[index];
        char *target = NULL;
        if (destination_is_directory) {
            char *base = copy_path_basename(source);
            if (base == NULL) {
                copy_print_error(source, "cannot allocate destination name for");
                failures++;
                continue;
            }
            target = copy_join_path(destination, base);
            free(base);
        } else {
            target = strdup(destination);
        }
        if (target == NULL) {
            copy_print_error(destination, "cannot allocate destination path for");
            failures++;
            continue;
        }
        bool duplicate_target = false;
        for (int seen = 0; seen < seen_count; seen++) {
            if (strcmp(target, seen_targets[seen]) == 0) {
                duplicate_target = true;
                break;
            }
        }
        if (duplicate_target) {
            fprintf(stderr,
                    "%s: multiple sources resolve to destination '%s'\n",
                    program_name, target);
            failures++;
            free(target);
            continue;
        }
        seen_targets[seen_count] = strdup(target);
        if (seen_targets[seen_count] == NULL) {
            copy_print_error(target, "cannot remember destination for");
            failures++;
            free(target);
            continue;
        }
        seen_count++;

        Progress progress = {
            .enabled = options->progress && !options->dry_run &&
                       isatty(STDERR_FILENO),
            .last_percent = -1
        };
        if (progress.enabled &&
            copy_scan_progress_tree(source, options->recursive, &progress) < 0) {
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
    for (int index = 0; index < seen_count; index++) {
        free(seen_targets[index]);
    }
    free(seen_targets);
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
            } else if (strcmp(arg, "--dry-run") == 0) {
                options.dry_run = true;
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
