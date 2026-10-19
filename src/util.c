#include "common.h"

#include "util.h"

void copy_print_error(const char *path, const char *action)
{
    fprintf(stderr, "%s: %s '%s': %s\n", program_name, action, path,
            strerror(errno));
}

void copy_print_dry_run(const char *action, const char *source,
                        const char *destination)
{
    if (destination == NULL) {
        printf("Would %s '%s'\n", action, source);
    } else {
        printf("Would %s '%s' -> '%s'\n", action, source, destination);
    }
}

char *copy_join_path(const char *left, const char *right)
{
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    bool separator_needed = left_len > 0 && left[left_len - 1] != '/';

    if (left_len > SIZE_MAX - right_len - (separator_needed ? 2 : 1)) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    size_t length = left_len + right_len + (separator_needed ? 1 : 0) + 1;
    char *joined = malloc(length);
    if (joined == NULL) {
        return NULL;
    }
    snprintf(joined, length, "%s%s%s", left,
             separator_needed ? "/" : "", right);
    return joined;
}

char *copy_path_basename(const char *path)
{
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') {
        length--;
    }
    size_t start = length;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }
    if (start == length) {
        return strdup("/");
    }
    size_t name_length = length - start;
    char *name = malloc(name_length + 1);
    if (name == NULL) {
        return NULL;
    }
    memcpy(name, path + start, name_length);
    name[name_length] = '\0';
    return name;
}

bool copy_same_file(const struct stat *source, const struct stat *destination)
{
    return source->st_dev == destination->st_dev &&
           source->st_ino == destination->st_ino;
}
