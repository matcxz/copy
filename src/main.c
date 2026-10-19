#include "common.h"

#include "options.h"
#include "progress.h"
#include "tree.h"
#include "util.h"

const char *program_name = "copy";

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "copy — An independent, lightweight alternative to cp.\n\n"
            "Usage: %s [OPTIONS] SOURCE... DESTINATION\n\n"
            "Options:\n"
            "  -R, -r, --recursive  Copy directories and their contents\n"
            "  -p, --preserve       Preserve file modes and modification times\n"
            "  -n, --no-clobber     Never overwrite an existing destination\n"
            "  -v, --verbose        Print each copied path\n"
            "  -c, --verify         Compare copied file contents with the source\n"
            "  -g, --progress       Show overall copy progress on the terminal\n"
            "      --resume         Resume regular-file copies; verify before publishing\n"
            "      --dry-run        Show planned changes without copying anything\n"
            "  -h, --help           Show this help\n"
            "  -V, --version        Show version information\n\n"
            "Symbolic links are copied as links. Multiple sources require an\n"
            "existing destination directory. Resume data is kept in a hidden\n"
            "file beside the destination until the copy is verified.\n",
            program_name);
}

static int main_copy(int argc, char **argv, const Options *options)
{
    int operand_count = argc;
    if (operand_count < 2) {
        print_usage(stderr);
        return 2;
    }

    const char *destination = argv[operand_count - 1];
    int source_count = operand_count - 1;
    struct stat destination_stat;
    bool destination_is_directory =
        stat(destination, &destination_stat) == 0 &&
        S_ISDIR(destination_stat.st_mode);
    if (source_count > 1 && !destination_is_directory) {
        fprintf(stderr,
                "%s: destination '%s' must be an existing directory when copying multiple sources\n",
                program_name, destination);
        return 2;
    }

    int failures = 0;
    char **seen_targets = calloc((size_t)source_count, sizeof *seen_targets);
    if (seen_targets == NULL) {
        perror(program_name);
        return 1;
    }
    int seen_count = 0;
    for (int index = 0; index < source_count; index++) {
        const char *source = argv[index];
        char *target = NULL;
        if (destination_is_directory) {
            char *base = copy_path_basename(source);
            if (base == NULL) {
                copy_print_error(source, "cannot allocate destination name for");
                failures++;
                continue;
            }
            target = copy_join_path(destination, base);
            free(base);
        } else {
            target = strdup(destination);
        }
        if (target == NULL) {
            copy_print_error(destination, "cannot allocate destination path for");
            failures++;
            continue;
        }
        bool duplicate_target = false;
        for (int seen = 0; seen < seen_count; seen++) {
            if (strcmp(target, seen_targets[seen]) == 0) {
                duplicate_target = true;
                break;
            }
        }
        if (duplicate_target) {
            fprintf(stderr,
                    "%s: multiple sources resolve to destination '%s'\n",
                    program_name, target);
            failures++;
            free(target);
            continue;
        }
        seen_targets[seen_count] = strdup(target);
        if (seen_targets[seen_count] == NULL) {
            copy_print_error(target, "cannot remember destination for");
            failures++;
            free(target);
            continue;
        }
        seen_count++;

        Progress progress = {
            .enabled = options->progress && !options->dry_run &&
                       isatty(STDERR_FILENO),
            .last_percent = -1
        };
        if (progress.enabled &&
            copy_scan_progress_tree(source, options->recursive, &progress) < 0) {
            failures++;
            free(target);
            continue;
        }
        if (copy_entry(source, target, options,
                       progress.enabled ? &progress : NULL) < 0) {
            failures++;
        }
        if (progress.last_percent >= 0) fputc('\n', stderr);
        free(target);
    }
    for (int index = 0; index < seen_count; index++) {
        free(seen_targets[index]);
    }
    free(seen_targets);
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    Options options = {0};
    char **operands = calloc((size_t)argc, sizeof *operands);
    if (operands == NULL) {
        perror(program_name);
        return 1;
    }
    int operand_count = 0;
    bool parse_options = true;

    for (int index = 1; index < argc; index++) {
        const char *arg = argv[index];
        if (parse_options && strcmp(arg, "--") == 0) {
            parse_options = false;
            continue;
        }
        if (parse_options && arg[0] == '-' && arg[1] != '-' &&
            arg[1] != '\0' && arg[2] != '\0') {
            for (const char *option = arg + 1; *option != '\0'; option++) {
                switch (*option) {
                case 'r':
                case 'R':
                    options.recursive = true;
                    break;
                case 'p':
                    options.preserve = true;
                    break;
                case 'n':
                    options.no_clobber = true;
                    break;
                case 'v':
                    options.verbose = true;
                    break;
                case 'c':
                    options.verify = true;
                    break;
                case 'g':
                    options.progress = true;
                    break;
                case 'h':
                    print_usage(stdout);
                    free(operands);
                    return 0;
                case 'V':
                    puts("copy 0.1.0");
                    free(operands);
                    return 0;
                default:
                    fprintf(stderr, "%s: unknown option: '-%c'\n",
                            program_name, *option);
                    print_usage(stderr);
                    free(operands);
                    return 2;
                }
            }
            continue;
        }
        if (parse_options && arg[0] == '-' && arg[1] != '\0') {
            if (strcmp(arg, "-R") == 0 || strcmp(arg, "-r") == 0 ||
                strcmp(arg, "--recursive") == 0) {
                options.recursive = true;
            } else if (strcmp(arg, "-p") == 0 ||
                       strcmp(arg, "--preserve") == 0) {
                options.preserve = true;
            } else if (strcmp(arg, "-n") == 0 ||
                       strcmp(arg, "--no-clobber") == 0) {
                options.no_clobber = true;
            } else if (strcmp(arg, "-v") == 0 ||
                       strcmp(arg, "--verbose") == 0) {
                options.verbose = true;
            } else if (strcmp(arg, "-c") == 0 ||
                       strcmp(arg, "--verify") == 0) {
                options.verify = true;
            } else if (strcmp(arg, "-g") == 0 ||
                       strcmp(arg, "--progress") == 0) {
                options.progress = true;
            } else if (strcmp(arg, "--resume") == 0) {
                options.resume = true;
            } else if (strcmp(arg, "--dry-run") == 0) {
                options.dry_run = true;
            } else if (strcmp(arg, "-h") == 0 ||
                       strcmp(arg, "--help") == 0) {
                print_usage(stdout);
                free(operands);
                return 0;
            } else if (strcmp(arg, "-V") == 0 ||
                       strcmp(arg, "--version") == 0) {
                puts("copy 0.1.0");
                free(operands);
                return 0;
            } else {
                fprintf(stderr, "%s: unknown option: '%s'\n",
                        program_name, arg);
                print_usage(stderr);
                free(operands);
                return 2;
            }
        } else {
            operands[operand_count++] = argv[index];
        }
    }

    if (operand_count < 2) {
        print_usage(stderr);
        free(operands);
        return 2;
    }
    int result = main_copy(operand_count, operands, &options);
    free(operands);
    return result;
}
