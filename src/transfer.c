#include "common.h"

#include "progress.h"
#include "transfer.h"
#include "util.h"

int copy_transfer_range_buffered(int source_fd, int destination_fd,
                                 off_t source_offset, off_t destination_offset,
                                 off_t length, const char *source,
                                 const char *destination, Progress *progress)
{
    unsigned char buffer[128 * 1024];
    off_t remaining = length;
    while (remaining > 0) {
        size_t requested = (uintmax_t)remaining > sizeof buffer
                               ? sizeof buffer
                               : (size_t)remaining;
        ssize_t count = pread(source_fd, buffer, requested, source_offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            copy_print_error(source, "cannot read");
            return -1;
        }
        if (count == 0) {
            errno = EIO;
            copy_print_error(source, "cannot read");
            return -1;
        }

        ssize_t written_total = 0;
        while (written_total < count) {
            ssize_t written = pwrite(destination_fd, buffer + written_total,
                                     (size_t)(count - written_total),
                                     destination_offset + written_total);
            if (written < 0) {
                if (errno == EINTR) continue;
                copy_print_error(destination, "cannot write");
                return -1;
            }
            if (written == 0) {
                errno = EIO;
                copy_print_error(destination, "cannot write");
                return -1;
            }
            written_total += written;
        }
        source_offset += count;
        destination_offset += count;
        remaining -= count;
        copy_progress_add_bytes(progress, (uintmax_t)count);
    }
    return 0;
}

int copy_transfer_range(int source_fd, int destination_fd, off_t source_offset,
                        off_t destination_offset, off_t length,
                        const char *source, const char *destination,
                        Progress *progress)
{
    off_t remaining = length;
    while (remaining > 0) {
        size_t requested = (uintmax_t)remaining > 1024 * 1024
                               ? 1024 * 1024
                               : (size_t)remaining;
        ssize_t copied = copy_file_range(source_fd, &source_offset,
                                         destination_fd, &destination_offset,
                                         requested, 0);
        if (copied < 0) {
            if (errno == EINTR) continue;
            if (errno == EXDEV || errno == ENOSYS || errno == EINVAL ||
                errno == EOPNOTSUPP || errno == ENOTSUP) {
                return copy_transfer_range_buffered(
                    source_fd, destination_fd, source_offset,
                    destination_offset, remaining, source, destination,
                    progress);
            }
            copy_print_error(destination, "cannot copy data to");
            return -1;
        }
        if (copied == 0) {
            return copy_transfer_range_buffered(source_fd, destination_fd,
                                                source_offset, destination_offset,
                                                remaining, source, destination,
                                                progress);
        }
        remaining -= copied;
        copy_progress_add_bytes(progress, (uintmax_t)copied);
    }
    return 0;
}

int copy_transfer_file_contents(int source_fd, int destination_fd, off_t start,
                                off_t file_size, const char *source,
                                const char *destination, Progress *progress)
{
    off_t offset = start;
    bool sparse_supported = true;
    while (offset < file_size) {
        off_t data_offset = lseek(source_fd, offset, SEEK_DATA);
        if (data_offset < 0) {
            if (errno == ENXIO) {
                copy_progress_add_bytes(progress,
                                        (uintmax_t)(file_size - offset));
                offset = file_size;
                break;
            }
            if (errno == EINVAL || errno == EOPNOTSUPP || errno == ENOTSUP) {
                sparse_supported = false;
                break;
            }
            copy_print_error(source, "cannot locate file data in");
            return -1;
        }
        if (data_offset >= file_size) {
            copy_progress_add_bytes(progress, (uintmax_t)(file_size - offset));
            offset = file_size;
            break;
        }

        off_t hole_offset = lseek(source_fd, data_offset, SEEK_HOLE);
        if (hole_offset < 0) {
            if (errno == EINVAL || errno == ENXIO || errno == EOPNOTSUPP ||
                errno == ENOTSUP) {
                sparse_supported = false;
                break;
            }
            copy_print_error(source, "cannot locate file hole in");
            return -1;
        }
        if (hole_offset <= data_offset) {
            errno = EIO;
            copy_print_error(source, "cannot locate file hole in");
            return -1;
        }
        if (hole_offset > file_size) hole_offset = file_size;
        copy_progress_add_bytes(progress, (uintmax_t)(data_offset - offset));
        if (copy_transfer_range(source_fd, destination_fd, data_offset,
                                data_offset, hole_offset - data_offset, source,
                                destination, progress) < 0) {
            return -1;
        }
        offset = hole_offset;
    }

    if (!sparse_supported &&
        copy_transfer_range_buffered(source_fd, destination_fd, offset, offset,
                                     file_size - offset, source, destination,
                                     progress) < 0) {
        return -1;
    }
    if (ftruncate(destination_fd, file_size) < 0) {
        copy_print_error(destination, "cannot set file size for");
        return -1;
    }
    return 0;
}

int copy_transfer_stream_contents(int source_fd, int destination_fd,
                                  const char *source, const char *destination,
                                  Progress *progress)
{
    unsigned char buffer[128 * 1024];
    for (;;) {
        ssize_t count = read(source_fd, buffer, sizeof buffer);
        if (count < 0) {
            if (errno == EINTR) continue;
            copy_print_error(source, "cannot read");
            return -1;
        }
        if (count == 0) return 0;

        ssize_t offset = 0;
        while (offset < count) {
            ssize_t written = write(destination_fd, buffer + offset,
                                    (size_t)(count - offset));
            if (written < 0) {
                if (errno == EINTR) continue;
                copy_print_error(destination, "cannot write");
                return -1;
            }
            if (written == 0) {
                errno = EIO;
                copy_print_error(destination, "cannot write");
                return -1;
            }
            offset += written;
        }
        copy_progress_add_bytes(progress, (uintmax_t)count);
    }
}
