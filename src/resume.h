#ifndef COPY_RESUME_H
#define COPY_RESUME_H

#include "options.h"
#include "progress.h"

typedef int (*CopyMetadataFunction)(int fd, const struct stat *source,
                                    const char *path, bool preserve);

int copy_regular_file_resumable(const char *source, const char *destination,
                                int source_fd,
                                const struct stat *source_stat,
                                const Options *options, Progress *progress,
                                CopyMetadataFunction set_metadata);

#endif
