#include "common.h"

#include "progress.h"
#include "util.h"

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

void copy_progress_add_bytes(Progress *progress, uintmax_t amount)
{
    if (progress == NULL || !progress->enabled) return;
    if (amount >= progress->total_bytes - progress->processed_bytes) {
        progress->processed_bytes = progress->total_bytes;
    } else {
        progress->processed_bytes += amount;
    }
    show_progress(progress);
}

void copy_progress_mark_file(Progress *progress)
{
    if (progress == NULL || !progress->enabled) return;
    if (progress->processed_files < progress->total_files) {
        progress->processed_files++;
    }
    if (progress->total_bytes == 0) {
        show_progress(progress);
    }
}

int copy_add_progress_file(Progress *progress, const struct stat *file_stat,
                           const char *path)
{
    if (file_stat->st_size < 0) {
        errno = EOVERFLOW;
        copy_print_error(path, "cannot determine size of");
        return -1;
    }
    uintmax_t size = (uintmax_t)file_stat->st_size;
    if (UINTMAX_MAX - progress->total_bytes < size ||
        progress->total_files == SIZE_MAX) {
        errno = EOVERFLOW;
        copy_print_error(path, "too many bytes or files to track for");
        return -1;
    }
    progress->total_bytes += size;
    progress->total_files++;
    return 0;
}

int copy_scan_progress_tree(const char *path, bool recursive,
                            Progress *progress)
{
    struct stat file_stat;
    if (lstat(path, &file_stat) < 0) {
        copy_print_error(path, "cannot access while calculating progress for");
        return -1;
    }
    if (S_ISREG(file_stat.st_mode) || S_ISLNK(file_stat.st_mode)) {
        return copy_add_progress_file(progress, &file_stat, path);
    }
    if (!S_ISDIR(file_stat.st_mode) || !recursive) {
        return 0;
    }

    DIR *directory = opendir(path);
    if (directory == NULL) {
        copy_print_error(path, "cannot open while calculating progress for");
        return -1;
    }

    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (entry == NULL) {
            if (errno != 0) {
                copy_print_error(path, "cannot read while calculating progress for");
                result = -1;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *child = copy_join_path(path, entry->d_name);
        if (child == NULL) {
            copy_print_error(path, "cannot allocate progress path for");
            result = -1;
            continue;
        }
        if (copy_scan_progress_tree(child, recursive, progress) < 0) {
            result = -1;
        }
        free(child);
    }
    if (closedir(directory) < 0) {
        copy_print_error(path, "cannot close while calculating progress for");
        result = -1;
    }
    return result;
}
