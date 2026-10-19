#ifndef COPY_FILE_H
#define COPY_FILE_H

#include "options.h"
#include "progress.h"

int copy_regular_file(const char *source, const char *destination,
                      const struct stat *source_stat, const Options *options,
                      Progress *progress);
int copy_symlink(const char *source, const char *destination,
                 const struct stat *source_stat, const Options *options,
                 Progress *progress);

#endif
