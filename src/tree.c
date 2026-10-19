#include "common.h"

#include "file.h"
#include "options.h"
#include "progress.h"
#include "tree.h"
#include "util.h"

int copy_entry(const char *source, const char *destination,
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

int copy_entry(const char *source, const char *destination,
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
