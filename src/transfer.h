#ifndef COPY_TRANSFER_H
#define COPY_TRANSFER_H

#include "progress.h"

int copy_transfer_range_buffered(int source_fd, int destination_fd,
                                off_t source_offset, off_t destination_offset,
                                off_t length, const char *source,
                                const char *destination, Progress *progress);
int copy_transfer_range(int source_fd, int destination_fd, off_t source_offset,
                        off_t destination_offset, off_t length,
                        const char *source, const char *destination,
                        Progress *progress);
int copy_transfer_file_contents(int source_fd, int destination_fd, off_t start,
                                off_t file_size, const char *source,
                                const char *destination, Progress *progress);
int copy_transfer_stream_contents(int source_fd, int destination_fd,
                                 const char *source, const char *destination,
                                 Progress *progress);

#endif
