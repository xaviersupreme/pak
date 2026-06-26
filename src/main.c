#include "pak.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out, "usage:\n");
    fprintf(out, "  pak make [options] <archive.pak> <files...>\n");
    fprintf(out, "  pak list [options] <archive.pak>\n");
    fprintf(out, "  pak extract [options] <archive.pak> [files...]\n");
    fprintf(out, "  pak cat <archive.pak> <file>\n");
    fprintf(out, "  pak info <archive.pak>\n");
    fprintf(out, "  pak verify|test <archive.pak>\n");
    fprintf(out, "\noptions:\n");
    fprintf(out, "  --compress               compress entries when useful\n");
    fprintf(out, "  --paths                  keep relative paths\n");
    fprintf(out, "  --long                   detailed list output\n");
    fprintf(out, "  -C <dir>, -C<dir>        extract into directory\n");
    fprintf(out, "  --overwrite              replace existing files when extracting\n");
    fprintf(out, "  --skip-existing          skip existing files when extracting\n");
    fprintf(out, "  --                       stop parsing options\n");
}

static int is_help_flag(const char *arg)
{
    return strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0;
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

static int ascii_lower(int ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 'a';
    }

    return ch;
}

static int has_pak_extension(const char *path)
{
    size_t len = strlen(path);
    const char *ext;

    if (len < 4) {
        return 0;
    }

    ext = path + len - 4;
    return ascii_lower(ext[0]) == '.' && ascii_lower(ext[1]) == 'p' && ascii_lower(ext[2]) == 'a' && ascii_lower(ext[3]) == 'k';
}

static char *make_archive_path(const char *path)
{
    char *out;
    size_t len;

    len = strlen(path);
    out = malloc(len + (has_pak_extension(path) ? 1 : 5));
    if (out == NULL) {
        return NULL;
    }

    strcpy(out, path);
    if (!has_pak_extension(path)) {
        strcat(out, ".pak");
    }

    return out;
}

static int parse_args(int argc, char **argv, char **args, int *count, struct pak_options *opts)
{
    const char *command;
    int parsing_options;
    int i;

    command = NULL;
    parsing_options = 1;
    *count = 0;
    memset(opts, 0, sizeof(*opts));
    opts->overwrite_mode = PAK_OVERWRITE_REFUSE;

    for (i = 1; i < argc; i++) {
        if (parsing_options && strcmp(argv[i], "--") == 0) {
            parsing_options = 0;
        } else if (parsing_options && strcmp(argv[i], "--paths") == 0) {
            opts->preserve_paths = 1;
        } else if (parsing_options && strcmp(argv[i], "--compress") == 0) {
            opts->compress = 1;
        } else if (parsing_options && strcmp(argv[i], "--long") == 0) {
            opts->long_list = 1;
        } else if (parsing_options && strcmp(argv[i], "--overwrite") == 0) {
            opts->overwrite_mode = PAK_OVERWRITE_REPLACE;
        } else if (parsing_options && strcmp(argv[i], "--skip-existing") == 0) {
            opts->overwrite_mode = PAK_OVERWRITE_SKIP;
        } else if (parsing_options && strcmp(argv[i], "-C") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "pak: -C needs a directory\n");
                return -1;
            }
            opts->extract_dir = argv[++i];
        } else if (parsing_options && strncmp(argv[i], "-C", 2) == 0 && argv[i][2] != '\0') {
            opts->extract_dir = argv[i] + 2;
        } else if (command == NULL && is_command(argv[i])) {
            command = argv[i];
            push_arg(args, count, argv[i]);
        } else if (parsing_options && argv[i][0] == '-') {
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
    int i;

    for (i = 1; i < argc; i++) {
        if (is_help_flag(argv[i])) {
            usage(stdout);
            return 0;
        }
    }

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
            char *archive_path = make_archive_path(args[1]);

            if (archive_path == NULL) {
                fprintf(stderr, "pak: out of memory\n");
                rc = 1;
            } else {
                rc = pak_make(archive_path, count - 2, &args[2], &opts);
                free(archive_path);
            }
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
