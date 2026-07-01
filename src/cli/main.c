#include "pak.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage_section(FILE *out, const char *name)
{
    fprintf(out, "\n%s%s:%s\n", pak_clr(out, PAK_CLR_BOLD PAK_CLR_CYAN), name, pak_clr(out, PAK_CLR_RESET));
}

static void usage_command(FILE *out, const char *command, const char *args)
{
    fprintf(out, "  %spak%s %s%-7s%s %s%s%s\n",
        pak_clr(out, PAK_CLR_DIM),
        pak_clr(out, PAK_CLR_RESET),
        pak_clr(out, PAK_CLR_BOLD PAK_CLR_GREEN),
        command,
        pak_clr(out, PAK_CLR_RESET),
        pak_clr(out, PAK_CLR_DIM),
        args,
        pak_clr(out, PAK_CLR_RESET));
}

static void usage_flag(FILE *out, const char *flag, const char *desc)
{
    fprintf(out, "  %s%-27s%s %s\n",
        pak_clr(out, PAK_CLR_GREEN),
        flag,
        pak_clr(out, PAK_CLR_RESET),
        desc);
}

static void usage(FILE *out)
{
    fprintf(out, "%susage:%s\n", pak_clr(out, PAK_CLR_BOLD PAK_CLR_CYAN), pak_clr(out, PAK_CLR_RESET));
    usage_command(out, "make", "[options] <archive.pak> <files...>");
    usage_command(out, "update", "[options] <archive.pak> <files...>");
    usage_command(out, "list", "[options] <archive.pak> [files...|patterns...]");
    usage_command(out, "extract", "[options] <archive.pak> [files...|patterns...]");
    usage_command(out, "unpack", "[options] <archive.pak> [files...|patterns...]");
    usage_command(out, "cat", "<archive.pak> <file|pattern>");
    usage_command(out, "info", "<archive.pak>");
    usage_command(out, "delete", "<archive.pak> <files...|patterns...>");
    usage_command(out, "rename", "<archive.pak> <old> <new>");
    usage_command(out, "repack", "[options] <archive.pak> [files...|patterns...]");
    usage_command(out, "check", "<archive.pak>");
    usage_command(out, "version", "");

    fprintf(out, "\nflags are command scoped; they can appear before or after the command.\n");

    usage_section(out, "compression flags for make/update/repack");
    usage_flag(out, "--compress", "compress useful entries, skip weak wins on large files");
    usage_flag(out, "--no-smart-compress", "enable compression without file type or sample skipping");
    usage_flag(out, "--level <0..10>, -0..-9", "set deflate level and enable compression");

    usage_section(out, "make/update flags");
    usage_flag(out, "--paths", "keep relative paths");
    usage_flag(out, "--exclude <pattern>", "skip files while packing");
    usage_flag(out, "--no-pakignore", "ignore .pakignore");

    usage_section(out, "repack flags");
    usage_flag(out, "--store", "store entries without compression");

    usage_section(out, "list flags");
    usage_flag(out, "--long", "detailed list output");
    usage_flag(out, "--full-name", "show full names and enable long output");

    usage_section(out, "extract/unpack flags");
    usage_flag(out, "-C <dir>, -C<dir>", "extract into directory");
    usage_flag(out, "--overwrite", "replace existing files when extracting");
    usage_flag(out, "--skip-existing", "skip existing files when extracting");

    usage_section(out, "global flags");
    usage_flag(out, "-h, --help", "show help");
    usage_flag(out, "--", "stop parsing options");

    fprintf(out, "\ncheck validates data, scans damaged headers, and offers repair.\n");
}

static int is_help_flag(const char *arg)
{
    return strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0;
}

static int is_version_flag(const char *arg)
{
    return strcmp(arg, "--version") == 0 || strcmp(arg, "--v") == 0 || strcmp(arg, "-v") == 0 || strcmp(arg, "-V") == 0;
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

static void options_init(struct pak_options *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->compression_level = PAK_DEFAULT_COMPRESSION_LEVEL;
    opts->smart_compress = 1;
    opts->store = 0;
    opts->overwrite_mode = PAK_OVERWRITE_REFUSE;
    opts->use_pakignore = 1;
    opts->option_mask = 0;
    opts->seen_option_count = 0;
    pattern_list_init(&opts->exclude_patterns);
}

static void options_free(struct pak_options *opts)
{
    pattern_list_free(&opts->exclude_patterns);
}

static int parse_level_value(const char *value, int *level)
{
    char *end;
    long number;

    if (value == NULL || value[0] == '\0') {
        return -1;
    }

    errno = 0;
    number = strtol(value, &end, 10);
    if (errno != 0 || *end != '\0' || number < 0 || number > 10) {
        return -1;
    }

    *level = (int)number;
    return 0;
}

static int set_compression_level(struct pak_options *opts, const char *value)
{
    int level;

    if (parse_level_value(value, &level) != 0) {
        hint_bad_compression_level(value);
        return -1;
    }

    opts->compress = 1;
    opts->compression_level = level;
    return 0;
}

static int add_exclude_pattern(struct pak_options *opts, const char *pattern)
{
    if (pattern_list_add(&opts->exclude_patterns, pattern) != 0) {
        diag_error("could not add exclude pattern '%s'", pattern);
        return -1;
    }
    return 0;
}

static int parse_args(int argc, char **argv, char **args, int *count, struct pak_options *opts)
{
    const char *command;
    int parsing_options;
    int i;

    command = NULL;
    parsing_options = 1;
    *count = 0;
    options_init(opts);

    for (i = 1; i < argc; i++) {
        if (parsing_options && strcmp(argv[i], "--") == 0) {
            parsing_options = 0;
        } else if (parsing_options && strcmp(argv[i], "--paths") == 0) {
            opts->preserve_paths = 1;
            pak_note_option(opts, PAK_OPT_PATHS, argv[i]);
        } else if (parsing_options && strcmp(argv[i], "--compress") == 0) {
            opts->compress = 1;
            pak_note_option(opts, PAK_OPT_COMPRESS, argv[i]);
        } else if (parsing_options && (strcmp(argv[i], "--no-smart-compress") == 0 || strcmp(argv[i], "--no-smart-compression") == 0)) {
            opts->compress = 1;
            opts->smart_compress = 0;
            pak_note_option(opts, PAK_OPT_NO_SMART_COMPRESS, argv[i]);
        } else if (parsing_options && strcmp(argv[i], "--store") == 0) {
            opts->store = 1;
            pak_note_option(opts, PAK_OPT_STORE, argv[i]);
        } else if (parsing_options && strcmp(argv[i], "--level") == 0) {
            if (i + 1 >= argc) {
                hint_missing_option_value("--level", "a number from 0 to 10");
                return -1;
            }
            pak_note_option(opts, PAK_OPT_LEVEL, argv[i]);
            if (set_compression_level(opts, argv[++i]) != 0) {
                return -1;
            }
        } else if (parsing_options && strncmp(argv[i], "--level=", 8) == 0) {
            pak_note_option(opts, PAK_OPT_LEVEL, "--level");
            if (set_compression_level(opts, argv[i] + 8) != 0) {
                return -1;
            }
        } else if (parsing_options && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9' && argv[i][2] == '\0') {
            char level[2];

            level[0] = argv[i][1];
            level[1] = '\0';
            pak_note_option(opts, PAK_OPT_LEVEL, argv[i]);
            if (set_compression_level(opts, level) != 0) {
                return -1;
            }
        } else if (parsing_options && strcmp(argv[i], "--long") == 0) {
            opts->long_list = 1;
            pak_note_option(opts, PAK_OPT_LONG, argv[i]);
        } else if (parsing_options && strcmp(argv[i], "--full-name") == 0) {
            opts->full_names = 1;
            opts->long_list = 1;
            pak_note_option(opts, PAK_OPT_FULL_NAME, argv[i]);
        } else if (parsing_options && strcmp(argv[i], "--exclude") == 0) {
            if (i + 1 >= argc) {
                hint_missing_option_value("--exclude", "a pattern");
                return -1;
            }
            pak_note_option(opts, PAK_OPT_EXCLUDE, argv[i]);
            if (add_exclude_pattern(opts, argv[++i]) != 0) {
                return -1;
            }
        } else if (parsing_options && strncmp(argv[i], "--exclude=", 10) == 0) {
            pak_note_option(opts, PAK_OPT_EXCLUDE, "--exclude");
            if (add_exclude_pattern(opts, argv[i] + 10) != 0) {
                return -1;
            }
        } else if (parsing_options && strcmp(argv[i], "--no-pakignore") == 0) {
            opts->use_pakignore = 0;
            pak_note_option(opts, PAK_OPT_NO_PAKIGNORE, argv[i]);
        } else if (parsing_options && strcmp(argv[i], "--overwrite") == 0) {
            opts->overwrite_mode = PAK_OVERWRITE_REPLACE;
            pak_note_option(opts, PAK_OPT_OVERWRITE, argv[i]);
        } else if (parsing_options && strcmp(argv[i], "--skip-existing") == 0) {
            opts->overwrite_mode = PAK_OVERWRITE_SKIP;
            pak_note_option(opts, PAK_OPT_SKIP_EXISTING, argv[i]);
        } else if (parsing_options && strcmp(argv[i], "-C") == 0) {
            if (i + 1 >= argc) {
                hint_missing_option_value("-C", "a directory");
                return -1;
            }
            pak_note_option(opts, PAK_OPT_C, argv[i]);
            opts->extract_dir = argv[++i];
        } else if (parsing_options && strncmp(argv[i], "-C", 2) == 0 && argv[i][2] != '\0') {
            pak_note_option(opts, PAK_OPT_C, "-C");
            opts->extract_dir = argv[i] + 2;
        } else if (command == NULL && pak_command_spec(argv[i]) != NULL) {
            command = argv[i];
            push_arg(args, count, argv[i]);
        } else if (parsing_options && argv[i][0] == '-') {
            hint_unknown_option(argv[i], argc, argv, i);
            return -1;
        } else {
            push_arg(args, count, argv[i]);
        }
    }

    return 0;
}

static int load_pakignore(struct pak_options *opts)
{
    if (!opts->use_pakignore) {
        return 0;
    }
    if (pattern_list_load_file(&opts->exclude_patterns, ".pakignore") != 0) {
        diag_error(".pakignore: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int run_write_command(const char *command, int count, char **args, struct pak_options *opts)
{
    struct path_list files;
    struct pak_options write_opts;
    char *archive_path;
    int saw_directory;
    int rc;

    if (count < 3) {
        diag_error("internal command error");
        return 1;
    }

    archive_path = make_archive_path(args[1]);
    if (archive_path == NULL) {
        diag_error("out of memory");
        return 1;
    }

    if (load_pakignore(opts) != 0) {
        free(archive_path);
        return 1;
    }

    rc = 1;
    path_list_init(&files);
    if (path_list_add_inputs(&files, count - 2, &args[2], &saw_directory) == 0) {
        write_opts = *opts;
        if (strcmp(command, "make") == 0) {
            rc = pak_make(archive_path, files.count, files.items, files.names, &write_opts) == 0 ? 0 : 1;
        } else {
            rc = pak_update(archive_path, files.count, files.items, files.names, &write_opts) == 0 ? 0 : 1;
        }
    }

    path_list_free(&files);
    free(archive_path);
    return rc;
}

int main(int argc, char **argv)
{
    const struct pak_command_spec *spec;
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
        if (is_version_flag(argv[i])) {
            printf("pak %s\n", PAK_VERSION);
            return 0;
        }
    }

    args = calloc((size_t)argc, sizeof(*args));
    if (args == NULL) {
        diag_error("out of memory");
        return 1;
    }

    if (parse_args(argc, argv, args, &count, &opts) != 0) {
        options_free(&opts);
        free(args);
        return 1;
    }
    if (count == 0) {
        hint_no_command(argc, argv, &opts);
        options_free(&opts);
        free(args);
        return 1;
    }

    spec = pak_command_spec(args[0]);
    if (spec == NULL) {
        hint_unknown_command(argc, argv, count, args, &opts);
        rc = 1;
    } else if (hint_validate_command(spec, argc, argv, count, args, &opts) != 0) {
        rc = 1;
    } else {
        switch (spec->id) {
        case PAK_CMD_MAKE:
        case PAK_CMD_UPDATE:
            rc = run_write_command(args[0], count, args, &opts);
            break;
        case PAK_CMD_LIST:
            rc = pak_list(args[1], count - 2, &args[2], &opts);
            break;
        case PAK_CMD_EXTRACT:
            rc = pak_extract(args[1], count - 2, &args[2], &opts);
            break;
        case PAK_CMD_CAT:
            rc = pak_cat(args[1], args[2], &opts);
            break;
        case PAK_CMD_INFO:
            rc = pak_info(args[1], &opts);
            break;
        case PAK_CMD_DELETE:
            rc = pak_delete(args[1], count - 2, &args[2], &opts);
            break;
        case PAK_CMD_RENAME:
            rc = pak_rename(args[1], args[2], args[3], &opts);
            break;
        case PAK_CMD_REPACK:
            rc = pak_repack(args[1], count - 2, &args[2], &opts);
            break;
        case PAK_CMD_CHECK:
            rc = pak_check(args[1], &opts);
            break;
        case PAK_CMD_VERSION:
            printf("pak %s\n", PAK_VERSION);
            rc = 0;
            break;
        default:
            diag_error("internal command error");
            rc = 1;
            break;
        }
    }

    options_free(&opts);
    free(args);
    return rc == 0 ? 0 : 1;
}
