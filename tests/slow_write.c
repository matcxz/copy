#define _GNU_SOURCE

#include <stddef.h>
#include <time.h>
#include <unistd.h>

#include <sys/syscall.h>

ssize_t write(int file_descriptor, const void *buffer, size_t count)
{
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 3000000};
    nanosleep(&delay, NULL);
    return syscall(SYS_write, file_descriptor, buffer, count);
}

ssize_t pwrite(int file_descriptor, const void *buffer, size_t count,
               off_t offset)
{
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 3000000};
    nanosleep(&delay, NULL);
    return syscall(SYS_pwrite64, file_descriptor, buffer, count, offset);
}

ssize_t copy_file_range(int input_fd, off_t *input_offset, int output_fd,
                        off_t *output_offset, size_t length,
                        unsigned int flags)
{
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 3000000};
    nanosleep(&delay, NULL);
    return syscall(SYS_copy_file_range, input_fd, input_offset, output_fd,
                   output_offset, length, flags);
}
