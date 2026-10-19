#include "common.h"

#include "util.h"
#include "verify.h"

ssize_t copy_read_block_at(int fd, unsigned char *buffer, size_t capacity,
                           off_t offset, const char *path, const char *action)
{
    size_t total = 0;
    while (total < capacity) {
        ssize_t count = pread(fd, buffer + total, capacity - total,
                              offset + (off_t)total);
        if (count < 0) {
            if (errno == EINTR) continue;
            copy_print_error(path, action);
            return -1;
        }
        if (count == 0) break;
        total += (size_t)count;
    }
    return (ssize_t)total;
}

int copy_files_equal(int source_fd, int destination_fd, const char *source,
                     const char *destination)
{
    unsigned char source_buffer[128 * 1024];
    unsigned char destination_buffer[128 * 1024];
    off_t offset = 0;
    for (;;) {
        ssize_t source_count = copy_read_block_at(
            source_fd, source_buffer, sizeof source_buffer, offset, source,
            "cannot read while verifying");
        if (source_count < 0) return -1;
        ssize_t destination_count = copy_read_block_at(
            destination_fd, destination_buffer, sizeof destination_buffer,
            offset, destination, "cannot read while verifying");
        if (destination_count < 0) return -1;
        if (source_count != destination_count ||
            memcmp(source_buffer, destination_buffer,
                   (size_t)source_count) != 0) {
            return 0;
        }
        if (source_count == 0) {
            return 1;
        }
        offset += source_count;
    }
}

int copy_verify_contents(int source_fd, int destination_fd, const char *source,
                         const char *destination)
{
    int equal = copy_files_equal(source_fd, destination_fd, source,
                                 destination);
    if (equal == 0) {
        fprintf(stderr, "%s: verification failed for '%s'\n",
                program_name, destination);
        errno = EIO;
        return -1;
    }
    return equal;
}

int copy_prepare_disk_verification(int source_fd, int destination_fd,
                                   const char *source,
                                   const char *destination)
{
    if (fsync(destination_fd) < 0) {
        copy_print_error(destination, "cannot sync before verification");
        return -1;
    }
    int advice_result = posix_fadvise(destination_fd, 0, 0,
                                      POSIX_FADV_DONTNEED);
    if (advice_result != 0) {
        errno = advice_result;
        copy_print_error(destination, "cannot discard cached data for");
        return -1;
    }
    advice_result = posix_fadvise(source_fd, 0, 0, POSIX_FADV_DONTNEED);
    if (advice_result != 0) {
        errno = advice_result;
        copy_print_error(source, "cannot discard cached data for");
        return -1;
    }
    return 0;
}
