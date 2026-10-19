#ifndef COPY_VERIFY_H
#define COPY_VERIFY_H

#include "common.h"

ssize_t copy_read_block_at(int fd, unsigned char *buffer, size_t capacity,
                           off_t offset, const char *path, const char *action);
int copy_files_equal(int source_fd, int destination_fd, const char *source,
                     const char *destination);
int copy_verify_contents(int source_fd, int destination_fd, const char *source,
                         const char *destination);
int copy_prepare_disk_verification(int source_fd, int destination_fd,
                                   const char *source,
                                   const char *destination);

#endif
