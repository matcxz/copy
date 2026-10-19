#ifndef COPY_UTIL_H
#define COPY_UTIL_H

#include "common.h"

extern const char *program_name;

void copy_print_error(const char *path, const char *action);
void copy_print_dry_run(const char *action, const char *source,
                        const char *destination);
char *copy_join_path(const char *left, const char *right);
char *copy_path_basename(const char *path);
bool copy_same_file(const struct stat *source, const struct stat *destination);

#endif
