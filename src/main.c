#include "pak.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out, "usage:\n");
    fprintf(out, "  pak [--v|--verbose] make [--paths] [--compress] <archive.pak> <files...>\n");
    fprintf(out, "  pak [--v|--verbose] list [--long] <archive.pak>\n");
    fprintf(out, "  pak [--v|--verbose] extract [-C dir] [--overwrite|--skip-existing] <archive.pak> [files...]\n");
    fprintf(out, "  pak cat <archive.pak> <file>\n");
    fprintf(out, "  pak [--v|--verbose] info <archive.pak>\n");
    fprintf(out, "  pak [--v|--verbose] verify|test <archive.pak>\n");
}

static int is_verbose_flag(const char *arg)
{
    return strcmp(arg, "--v") == 0 || strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0;
}

static int is_command(const char *arg)
{
    return strcmp(arg, "make") == 0 || strcmp(arg, "list") == 0 || strcmp(arg, "extract") == 0 || strcmp(arg, "cat") == 0 || strcmp(arg, "info") == 0 || strcmp(arg, "verify") == 0 || strcmp(arg, "test") == 0;
}

static int push_arg(char **args, int *count, char *arg)
{
    args[*count] = arg;
    *count += 1;
    return 0;
}

static int parse_args(int argc, char **argv, char **args, int *count, struct pak_options *opts)
{
    const char *command;
    int i;

    command = NULL;
    *count = 0;
    memset(opts, 0, sizeof(*opts));
    opts->overwrite_mode = PAK_OVERWRITE_REFUSE;

    for (i = 1; i < argc; i++) {
        if (is_verbose_flag(argv[i])) {
            opts->verbose = 1;
        } else if (command == NULL && is_command(argv[i])) {
            command = argv[i];
            push_arg(args, count, argv[i]);
        } else if (command != NULL && strcmp(command, "make") == 0 && strcmp(argv[i], "--paths") == 0) {
            opts->preserve_paths = 1;
        } else if (command != NULL && strcmp(command, "make") == 0 && strcmp(argv[i], "--compress") == 0) {
            opts->compress = 1;
        } else if (command != NULL && strcmp(command, "list") == 0 && strcmp(argv[i], "--long") == 0) {
            opts->long_list = 1;
        } else if (command != NULL && strcmp(command, "extract") == 0 && strcmp(argv[i], "--overwrite") == 0) {
            opts->overwrite_mode = PAK_OVERWRITE_REPLACE;
        } else if (command != NULL && strcmp(command, "extract") == 0 && strcmp(argv[i], "--skip-existing") == 0) {
            opts->overwrite_mode = PAK_OVERWRITE_SKIP;
        } else if (command != NULL && strcmp(command, "extract") == 0 && strcmp(argv[i], "-C") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "pak: -C needs a directory\n");
                return -1;
            }
            opts->extract_dir = argv[++i];
        } else if (command != NULL && strcmp(command, "extract") == 0 && strncmp(argv[i], "-C", 2) == 0 && argv[i][2] != '\0') {
            opts->extract_dir = argv[i] + 2;
        } else if (argv[i][0] == '-' && command != NULL) {
            fprintf(stderr, "pak: unknown option '%s'\n", argv[i]);
            return -1;
        } else {
            push_arg(args, count, argv[i]);
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct pak_options opts;
    char **args;
    int count;
    int rc;

    args = calloc((size_t)argc, sizeof(*args));
    if (args == NULL) {
        fprintf(stderr, "pak: out of memory\n");
        return 1;
    }

    if (parse_args(argc, argv, args, &count, &opts) != 0 || count < 2) {
        usage(stderr);
        free(args);
        return 1;
    }

    if (strcmp(args[0], "make") == 0) {
        if (count < 3) {
            usage(stderr);
            rc = 1;
        } else {
            rc = pak_make(args[1], count - 2, &args[2], &opts);
        }
    } else if (strcmp(args[0], "list") == 0) {
        rc = count == 2 ? pak_list(args[1], &opts) : 1;
    } else if (strcmp(args[0], "extract") == 0) {
        rc = count >= 2 ? pak_extract(args[1], count - 2, &args[2], &opts) : 1;
    } else if (strcmp(args[0], "cat") == 0) {
        rc = count == 3 ? pak_cat(args[1], args[2], &opts) : 1;
    } else if (strcmp(args[0], "info") == 0) {
        rc = count == 2 ? pak_info(args[1], &opts) : 1;
    } else if (strcmp(args[0], "verify") == 0 || strcmp(args[0], "test") == 0) {
        rc = count == 2 ? pak_verify(args[1], &opts) : 1;
    } else {
        fprintf(stderr, "pak: unknown command '%s'\n", args[0]);
        rc = 1;
    }

    if (rc != 0 && ((strcmp(args[0], "list") == 0 && count != 2) || (strcmp(args[0], "extract") == 0 && count < 2) || (strcmp(args[0], "cat") == 0 && count != 3) || (strcmp(args[0], "info") == 0 && count != 2) || ((strcmp(args[0], "verify") == 0 || strcmp(args[0], "test") == 0) && count != 2))) {
        usage(stderr);
    }

    free(args);
    return rc == 0 ? 0 : 1;
}
