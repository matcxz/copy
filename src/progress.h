#ifndef COPY_PROGRESS_H
#define COPY_PROGRESS_H

#include "common.h"

typedef struct {
    bool enabled;
    uintmax_t total_bytes;
    uintmax_t processed_bytes;
    size_t total_files;
    size_t processed_files;
    int last_percent;
} Progress;

void copy_progress_add_bytes(Progress *progress, uintmax_t amount);
void copy_progress_mark_file(Progress *progress);
int copy_add_progress_file(Progress *progress, const struct stat *file_stat,
                           const char *path);
int copy_scan_progress_tree(const char *path, bool recursive,
                            Progress *progress);

#endif
