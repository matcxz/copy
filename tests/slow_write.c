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
