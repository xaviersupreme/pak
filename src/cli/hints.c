#include "pak.h"

#include <stdio.h>
#include <string.h>

#define COMPRESS_OPTIONS (PAK_OPT_COMPRESS | PAK_OPT_LEVEL | PAK_OPT_NO_SMART_COMPRESS)
#define WRITE_OPTIONS (COMPRESS_OPTIONS | PAK_OPT_PATHS | PAK_OPT_EXCLUDE | PAK_OPT_NO_PAKIGNORE)
#define EXTRACT_OPTIONS (PAK_OPT_C | PAK_OPT_OVERWRITE | PAK_OPT_SKIP_EXISTING)
#define REPACK_OPTIONS (COMPRESS_OPTIONS | PAK_OPT_STORE)

struct option_spec {
    const char *name;
    unsigned int bit;
};

static void print_try_start(void)
{
    diag_try_start();
    fputs("pak", stderr);
}

static const struct pak_command_spec command_specs[] = {
    { "make", "make", PAK_CMD_MAKE, 2, PAK_ARG_MANY, WRITE_OPTIONS, "pak make [options] <archive> <files...>" },
    { "update", "update", PAK_CMD_UPDATE, 2, PAK_ARG_MANY, WRITE_OPTIONS, "pak update [options] <archive> <files...>" },
    { "list", "list", PAK_CMD_LIST, 1, PAK_ARG_MANY, PAK_OPT_LONG | PAK_OPT_FULL_NAME, "pak list [--long] [--full-name] <archive.pak> [files...]" },
    { "extract", "extract", PAK_CMD_EXTRACT, 1, PAK_ARG_MANY, EXTRACT_OPTIONS, "pak extract [options] <archive.pak> [files...]" },
    { "unpack", "extract", PAK_CMD_EXTRACT, 1, PAK_ARG_MANY, EXTRACT_OPTIONS, "pak unpack [options] <archive.pak> [files...]" },
    { "cat", "cat", PAK_CMD_CAT, 2, 2, 0, "pak cat <archive.pak> <file>" },
    { "info", "info", PAK_CMD_INFO, 1, 1, 0, "pak info <archive.pak>" },
    { "delete", "delete", PAK_CMD_DELETE, 2, PAK_ARG_MANY, 0, "pak delete <archive.pak> <files...>" },
    { "rename", "rename", PAK_CMD_RENAME, 3, 3, 0, "pak rename <archive.pak> <old> <new>" },
    { "repack", "repack", PAK_CMD_REPACK, 1, PAK_ARG_MANY, REPACK_OPTIONS, "pak repack [options] <archive.pak> [files...]" },
    { "check", "check", PAK_CMD_CHECK, 1, 1, 0, "pak check <archive.pak>" }
};

static const struct option_spec option_specs[] = {
    { "--compress", PAK_OPT_COMPRESS },
    { "--no-smart-compress", PAK_OPT_NO_SMART_COMPRESS },
    { "--no-smart-compression", PAK_OPT_NO_SMART_COMPRESS },
    { "--store", PAK_OPT_STORE },
    { "--level", PAK_OPT_LEVEL },
    { "--paths", PAK_OPT_PATHS },
    { "--exclude", PAK_OPT_EXCLUDE },
    { "--no-pakignore", PAK_OPT_NO_PAKIGNORE },
    { "--long", PAK_OPT_LONG },
    { "--full-name", PAK_OPT_FULL_NAME },
    { "-C", PAK_OPT_C },
    { "--overwrite", PAK_OPT_OVERWRITE },
    { "--skip-existing", PAK_OPT_SKIP_EXISTING },
    { "--help", 0 },
    { "-h", 0 }
};

static int safe_arg_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '/' || ch == '\\' || ch == ':' || ch == '=';
}

static int arg_needs_quotes(const char *arg)
{
    if (arg == NULL || arg[0] == '\0') {
        return 1;
    }

    while (*arg != '\0') {
        if (!safe_arg_char(*arg)) {
            return 1;
        }
        arg++;
    }

    return 0;
}

static void print_arg(const char *arg)
{
    if (!arg_needs_quotes(arg)) {
        fputs(arg, stderr);
        return;
    }

    fputc('"', stderr);
    while (*arg != '\0') {
        if (*arg == '"' || *arg == '\\') {
            fputc('\\', stderr);
        }
        fputc(*arg, stderr);
        arg++;
    }
    fputc('"', stderr);
}

static void print_placeholder(const char *arg)
{
    diag_placeholder(arg);
}

static int same_text(const char *left, const char *right)
{
    return strcmp(left, right) == 0;
}

static int min3(int a, int b, int c)
{
    int min = a < b ? a : b;

    return min < c ? min : c;
}

static int edit_distance(const char *left, const char *right)
{
    int prev[64];
    int curr[64];
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    size_t i;
    size_t j;

    if (left_len >= 64 || right_len >= 64) {
        return 99;
    }

    for (j = 0; j <= right_len; j++) {
        prev[j] = (int)j;
    }

    for (i = 1; i <= left_len; i++) {
        curr[0] = (int)i;
        for (j = 1; j <= right_len; j++) {
            int cost = left[i - 1] == right[j - 1] ? 0 : 1;

            curr[j] = min3(prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost);
        }
        for (j = 0; j <= right_len; j++) {
            prev[j] = curr[j];
        }
    }

    return prev[right_len];
}

static int suggestion_is_same_option_family(const char *option, const char *suggestion)
{
    if (suggestion == NULL) {
        return 0;
    }
    if (option[0] == '-' && option[1] == '-' && !(suggestion[0] == '-' && suggestion[1] == '-')) {
        return 0;
    }
    if (option[0] == '-' && option[1] != '-' && suggestion[0] == '-' && suggestion[1] == '-') {
        return 0;
    }
    return 1;
}

static const char *closest_command(const char *word)
{
    const char *best_word = NULL;
    int best_score = 99;
    size_t i;

    for (i = 0; i < sizeof(command_specs) / sizeof(command_specs[0]); i++) {
        int score = edit_distance(word, command_specs[i].name);

        if (score < best_score) {
            best_score = score;
            best_word = command_specs[i].name;
        }
    }

    return best_score <= 2 ? best_word : NULL;
}

static const struct option_spec *closest_option(const char *word)
{
    const struct option_spec *best = NULL;
    int best_score = 99;
    size_t i;

    for (i = 0; i < sizeof(option_specs) / sizeof(option_specs[0]); i++) {
        int score = edit_distance(word, option_specs[i].name);

        if (score < best_score) {
            best_score = score;
            best = &option_specs[i];
        }
    }

    return best_score <= 2 ? best : NULL;
}

static int command_index(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (pak_command_spec(argv[i]) != NULL) {
            return i;
        }
    }
    return -1;
}

static int is_level_option_typo(const char *option)
{
    if (option[0] != '-' || option[1] != '-') {
        return 0;
    }
    if (option[2] >= '0' && option[2] <= '9' && option[3] == '\0') {
        return 1;
    }
    return option[2] == '1' && option[3] == '0' && option[4] == '\0';
}

static void print_replacement(const char *option, const char *value)
{
    print_arg(option);
    if (value != NULL) {
        fputc(' ', stderr);
        print_arg(value);
    }
}

static void print_fixed_argv(const char *option, const char *value, int argc, char **argv, int option_index)
{
    int cmd_index = command_index(argc, argv);
    int i;

    print_try_start();
    if (cmd_index > option_index) {
        fputc(' ', stderr);
        print_arg(argv[cmd_index]);
        for (i = 1; i < cmd_index; i++) {
            fputc(' ', stderr);
            if (i == option_index) {
                print_replacement(option, value);
            } else {
                print_arg(argv[i]);
            }
        }
        for (i = cmd_index + 1; i < argc; i++) {
            fputc(' ', stderr);
            print_arg(argv[i]);
        }
        fputc('\n', stderr);
        return;
    }

    for (i = 1; i < argc; i++) {
        fputc(' ', stderr);
        if (i == option_index) {
            print_replacement(option, value);
        } else {
            print_arg(argv[i]);
        }
    }
    fputc('\n', stderr);
}

static int path_looks_like_archive(const char *path)
{
    size_t len;

    if (path == NULL) {
        return 0;
    }
    len = strlen(path);
    if (len < 4) {
        return 0;
    }

    return (path[len - 4] == '.' &&
            (path[len - 3] == 'p' || path[len - 3] == 'P') &&
            (path[len - 2] == 'a' || path[len - 2] == 'A') &&
            (path[len - 1] == 'k' || path[len - 1] == 'K'));
}

static int arg_has_pattern_magic(const char *arg)
{
    if (arg == NULL) {
        return 0;
    }

    while (*arg != '\0') {
        if (*arg == '*' || *arg == '?' || *arg == '[') {
            return 1;
        }
        arg++;
    }

    return 0;
}

static int arg_looks_like_archive_path(const char *arg)
{
    return path_looks_like_archive(arg) && !arg_has_pattern_magic(arg);
}

static int command_takes_archive_first(const struct pak_command_spec *spec)
{
    return spec->id == PAK_CMD_LIST ||
           spec->id == PAK_CMD_EXTRACT ||
           spec->id == PAK_CMD_CAT ||
           spec->id == PAK_CMD_INFO ||
           spec->id == PAK_CMD_DELETE ||
           spec->id == PAK_CMD_RENAME ||
           spec->id == PAK_CMD_REPACK ||
           spec->id == PAK_CMD_CHECK;
}

static int find_later_archive_arg(int count, char **args)
{
    int i;

    for (i = 2; i < count; i++) {
        if (arg_looks_like_archive_path(args[i])) {
            return i;
        }
    }

    return -1;
}

static const char *fixed_exclude_pattern(const char *pattern)
{
    if (same_text(pattern, "git") && io_file_exists(".git")) {
        return ".git/";
    }
    return pattern;
}

static void print_write_options(const struct pak_options *opts)
{
    int i;

    if (opts == NULL) {
        return;
    }
    if (opts->compress && opts->smart_compress && opts->compression_level == PAK_DEFAULT_COMPRESSION_LEVEL) {
        fputs(" --compress", stderr);
    }
    if (opts->compression_level != PAK_DEFAULT_COMPRESSION_LEVEL) {
        fprintf(stderr, " --level %d", opts->compression_level);
    }
    if (!opts->smart_compress) {
        fputs(" --no-smart-compress", stderr);
    }
    if (opts->preserve_paths) {
        fputs(" --paths", stderr);
    }
    if (!opts->use_pakignore) {
        fputs(" --no-pakignore", stderr);
    }

    for (i = 0; i < opts->exclude_patterns.count; i++) {
        fputs(" --exclude ", stderr);
        print_arg(fixed_exclude_pattern(opts->exclude_patterns.items[i]));
    }
}

static void print_extract_options(const struct pak_options *opts)
{
    if (opts == NULL) {
        return;
    }
    if (opts->overwrite_mode == PAK_OVERWRITE_REPLACE) {
        fputs(" --overwrite", stderr);
    } else if (opts->overwrite_mode == PAK_OVERWRITE_SKIP) {
        fputs(" --skip-existing", stderr);
    }
    if (opts->extract_dir != NULL && !path_looks_like_archive(opts->extract_dir)) {
        fputs(" -C ", stderr);
        print_arg(opts->extract_dir);
    }
}

static void print_repack_options(const struct pak_options *opts)
{
    if (opts == NULL) {
        return;
    }
    if (opts->store) {
        fputs(" --store", stderr);
        return;
    }
    if (opts->compression_level != PAK_DEFAULT_COMPRESSION_LEVEL) {
        fprintf(stderr, " --level %d", opts->compression_level);
    } else if (opts->compress && opts->smart_compress) {
        fputs(" --compress", stderr);
    }
    if (!opts->smart_compress) {
        fputs(" --no-smart-compress", stderr);
    }
}

static void print_allowed_options(const struct pak_command_spec *spec, const struct pak_options *opts)
{
    if (opts == NULL) {
        return;
    }
    if (spec->id == PAK_CMD_REPACK) {
        print_repack_options(opts);
    } else if ((spec->allowed_options & WRITE_OPTIONS) != 0) {
        print_write_options(opts);
    } else if ((spec->allowed_options & (PAK_OPT_LONG | PAK_OPT_FULL_NAME)) != 0) {
        if (opts->full_names) {
            fputs(" --full-name", stderr);
        } else if (opts->long_list) {
            fputs(" --long", stderr);
        }
    } else if ((spec->allowed_options & EXTRACT_OPTIONS) != 0) {
        print_extract_options(opts);
    }
}

static void print_command_start(const struct pak_command_spec *spec, const struct pak_options *opts)
{
    print_try_start();
    fprintf(stderr, " %s", spec->name);
    print_allowed_options(spec, opts);
}

static void print_positionals(int start, int count, char **args)
{
    int i;

    for (i = start; i < count; i++) {
        fputc(' ', stderr);
        print_arg(args[i]);
    }
}

static void print_reordered_archive_command(const struct pak_command_spec *spec, int count, char **args, const struct pak_options *opts, int archive_index)
{
    int i;

    print_command_start(spec, opts);
    fputc(' ', stderr);
    print_arg(args[archive_index]);
    for (i = 1; i < count; i++) {
        if (i == archive_index) {
            continue;
        }
        fputc(' ', stderr);
        print_arg(args[i]);
    }
    fputc('\n', stderr);
}

static const char *option_owner(unsigned int bit)
{
    if ((bit & COMPRESS_OPTIONS) != 0) {
        return "make/update/repack";
    }
    if ((bit & (PAK_OPT_PATHS | PAK_OPT_EXCLUDE | PAK_OPT_NO_PAKIGNORE)) != 0) {
        return "make/update";
    }
    if (bit == PAK_OPT_STORE) {
        return "repack";
    }
    if ((bit & EXTRACT_OPTIONS) != 0) {
        return "extract/unpack";
    }
    if (bit == PAK_OPT_LONG || bit == PAK_OPT_FULL_NAME) {
        return "list";
    }
    return NULL;
}

static int is_archive_only_command(const struct pak_command_spec *spec)
{
    return spec->id == PAK_CMD_INFO || spec->id == PAK_CMD_CHECK;
}

static const struct pak_seen_option *first_invalid_option(const struct pak_command_spec *spec, const struct pak_options *opts)
{
    int i;

    if (opts == NULL) {
        return NULL;
    }
    for (i = 0; i < opts->seen_option_count; i++) {
        if ((opts->seen_options[i].bit & spec->allowed_options) == 0) {
            return &opts->seen_options[i];
        }
    }
    return NULL;
}

static void hint_invalid_option(const struct pak_command_spec *spec, const struct pak_seen_option *option, int count, char **args, const struct pak_options *opts)
{
    const char *owner = option_owner(option->bit);

    diag_error("%s: option '%s' does not apply", spec->name, option->token);
    if (owner != NULL) {
        diag_hint("%s belongs to %s", option->token, owner);
    }

    print_command_start(spec, opts);
    print_positionals(1, count, args);
    fputc('\n', stderr);
}

static void print_git_hint_if_needed(const struct pak_options *opts)
{
    int i;

    if (opts == NULL || !io_file_exists(".git")) {
        return;
    }

    for (i = 0; i < opts->exclude_patterns.count; i++) {
        if (same_text(opts->exclude_patterns.items[i], "git")) {
            diag_hint("use .git/ to skip the Git metadata directory");
            return;
        }
    }
}

static void hint_missing_write_arg(const struct pak_command_spec *spec, int count, char **args, const struct pak_options *opts)
{
    if (count == 1) {
        diag_error("%s: missing archive name and input files", spec->name);
        print_command_start(spec, opts);
        fputc(' ', stderr);
        print_placeholder("<archive.pak>");
        fputs(" .\n", stderr);
        return;
    }

    diag_error("%s: missing input files", spec->name);
    diag_hint_start();
    fprintf(stderr, "%s needs files or a directory after ", spec->name);
    print_arg(args[1]);
    fputc('\n', stderr);
    if (opts != NULL && opts->exclude_patterns.count > 0) {
        diag_hint("--exclude only filters inputs; it does not add files");
        print_git_hint_if_needed(opts);
    }

    print_command_start(spec, opts);
    fputc(' ', stderr);
    print_arg(args[1]);
    fputs(" .\n", stderr);
}

static void hint_missing_extract_arg(const struct pak_command_spec *spec, const struct pak_options *opts)
{
    diag_error("%s: missing archive name", spec->name);
    if (path_looks_like_archive(opts == NULL ? NULL : opts->extract_dir)) {
        diag_hint_start();
        fprintf(stderr, "-C used ");
        print_arg(opts->extract_dir);
        fprintf(stderr, " as the output directory\n");
        print_try_start();
        fprintf(stderr, " %s ", spec->name);
        print_arg(opts->extract_dir);
        fputs(" -C out\n", stderr);
        return;
    }

    print_try_start();
    fprintf(stderr, " %s ", spec->name);
    print_placeholder("<archive.pak>");
    fputs(" -C out\n", stderr);
}

static void hint_missing_cat_arg(const struct pak_command_spec *spec, int count, char **args)
{
    if (count == 1) {
        diag_error("cat: missing archive name and file name");
        print_try_start();
        fputs(" cat ", stderr);
        print_placeholder("<archive.pak>");
        fputc(' ', stderr);
        print_placeholder("<file>");
        fputc('\n', stderr);
        return;
    }

    if (arg_has_pattern_magic(args[1]) && !arg_looks_like_archive_path(args[1])) {
        diag_error("cat: missing archive name");
        diag_hint("patterns go after the archive name");
        print_try_start();
        fprintf(stderr, " %s ", spec->name);
        print_placeholder("<archive.pak>");
        fputc(' ', stderr);
        print_arg(args[1]);
        fputc('\n', stderr);
        return;
    }

    diag_error("cat: missing file name");
    diag_hint("cat prints one file from an archive");
    print_try_start();
    fprintf(stderr, " %s ", spec->name);
    print_arg(args[1]);
    fputc(' ', stderr);
    print_placeholder("<file>");
    fputc('\n', stderr);
}

static void hint_missing_delete_arg(const struct pak_command_spec *spec, int count, char **args, const struct pak_options *opts)
{
    if (count == 1) {
        diag_error("delete: missing archive name and file name");
        print_try_start();
        fputs(" delete ", stderr);
        print_placeholder("<archive.pak>");
        fputc(' ', stderr);
        print_placeholder("<file>");
        fputc('\n', stderr);
        return;
    }

    if (arg_has_pattern_magic(args[1]) && !arg_looks_like_archive_path(args[1])) {
        diag_error("delete: missing archive name");
        diag_hint("patterns go after the archive name");
        print_command_start(spec, opts);
        fputc(' ', stderr);
        print_placeholder("<archive.pak>");
        fputc(' ', stderr);
        print_arg(args[1]);
        fputc('\n', stderr);
        return;
    }

    diag_error("delete: missing file name or pattern");
    diag_hint("delete removes entries from an archive");
    print_command_start(spec, opts);
    fputc(' ', stderr);
    print_arg(args[1]);
    fputc(' ', stderr);
    print_placeholder("<file>");
    fputc('\n', stderr);
}

static void hint_missing_rename_arg(const struct pak_command_spec *spec, int count, char **args, const struct pak_options *opts)
{
    if (count == 1) {
        diag_error("rename: missing archive name, old name, and new name");
        print_command_start(spec, opts);
        fputc(' ', stderr);
        print_placeholder("<archive.pak>");
        fputc(' ', stderr);
        print_placeholder("<old>");
        fputc(' ', stderr);
        print_placeholder("<new>");
        fputc('\n', stderr);
        return;
    }
    if (count == 2) {
        diag_error("rename: missing old name and new name");
        print_command_start(spec, opts);
        fputc(' ', stderr);
        print_arg(args[1]);
        fputc(' ', stderr);
        print_placeholder("<old>");
        fputc(' ', stderr);
        print_placeholder("<new>");
        fputc('\n', stderr);
        return;
    }

    diag_error("rename: missing new name");
    print_command_start(spec, opts);
    fputc(' ', stderr);
    print_arg(args[1]);
    fputc(' ', stderr);
    print_arg(args[2]);
    fputc(' ', stderr);
    print_placeholder("<new>");
    fputc('\n', stderr);
}

static void hint_missing_archive_arg(const struct pak_command_spec *spec, const struct pak_options *opts)
{
    diag_error("%s: missing archive name", spec->name);
    print_command_start(spec, opts);
    fputc(' ', stderr);
    print_placeholder("<archive.pak>");
    fputc('\n', stderr);
}

static void hint_missing_archive_before_entry(const struct pak_command_spec *spec, int count, char **args, const struct pak_options *opts)
{
    diag_error("%s: missing archive name", spec->name);
    diag_hint("file names and patterns go after the archive name");
    print_command_start(spec, opts);
    fputc(' ', stderr);
    print_placeholder("<archive.pak>");
    print_positionals(1, count, args);
    fputc('\n', stderr);
}

static void hint_missing_arg(const struct pak_command_spec *spec, int count, char **args, const struct pak_options *opts)
{
    if (spec->id == PAK_CMD_MAKE || spec->id == PAK_CMD_UPDATE) {
        hint_missing_write_arg(spec, count, args, opts);
    } else if (spec->id == PAK_CMD_EXTRACT) {
        hint_missing_extract_arg(spec, opts);
    } else if (spec->id == PAK_CMD_CAT) {
        hint_missing_cat_arg(spec, count, args);
    } else if (spec->id == PAK_CMD_DELETE) {
        hint_missing_delete_arg(spec, count, args, opts);
    } else if (spec->id == PAK_CMD_RENAME) {
        hint_missing_rename_arg(spec, count, args, opts);
    } else {
        hint_missing_archive_arg(spec, opts);
    }
}

static void hint_extra_cat_arg(const struct pak_command_spec *spec, int count, char **args)
{
    diag_error("cat: too many file names");
    diag_hint("cat prints one file; use unpack for multiple files");
    print_try_start();
    fputs(" unpack ", stderr);
    print_arg(args[1]);
    print_positionals(2, count, args);
    fputc('\n', stderr);
    (void)spec;
}

static void hint_extra_archive_arg(const struct pak_command_spec *spec, int count, char **args, const struct pak_options *opts)
{
    diag_error("%s: too many arguments", spec->name);
    if (is_archive_only_command(spec)) {
        diag_hint("%s only accepts one archive", spec->name);
    }
    print_command_start(spec, opts);
    if (count > 1) {
        fputc(' ', stderr);
        print_arg(args[1]);
    }
    fputc('\n', stderr);
}

static void hint_too_many_args(const struct pak_command_spec *spec, int count, char **args, const struct pak_options *opts)
{
    if (spec->id == PAK_CMD_CAT) {
        hint_extra_cat_arg(spec, count, args);
    } else if (spec->id == PAK_CMD_RENAME) {
        diag_error("rename: too many arguments");
        diag_hint("rename changes one entry name at a time");
        print_command_start(spec, opts);
        fputc(' ', stderr);
        print_arg(args[1]);
        fputc(' ', stderr);
        print_arg(args[2]);
        fputc(' ', stderr);
        print_arg(args[3]);
        fputc('\n', stderr);
    } else {
        hint_extra_archive_arg(spec, count, args, opts);
    }
}

static int hint_archive_arg_order(const struct pak_command_spec *spec, int count, char **args, const struct pak_options *opts)
{
    int archive_index;

    if (!command_takes_archive_first(spec) || count <= 1 || arg_looks_like_archive_path(args[1])) {
        return 0;
    }

    archive_index = find_later_archive_arg(count, args);
    if (archive_index >= 0) {
        diag_error("%s: archive name comes first", spec->name);
        if (is_archive_only_command(spec)) {
            diag_hint("%s only accepts one archive", spec->name);
            print_command_start(spec, opts);
            fputc(' ', stderr);
            print_arg(args[archive_index]);
            fputc('\n', stderr);
        } else {
            diag_hint("move %s before the file names or patterns", args[archive_index]);
            print_reordered_archive_command(spec, count, args, opts, archive_index);
        }
        return -1;
    }

    if (arg_has_pattern_magic(args[1])) {
        hint_missing_archive_before_entry(spec, count, args, opts);
        return -1;
    }

    return 0;
}

const struct pak_command_spec *pak_command_spec(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(command_specs) / sizeof(command_specs[0]); i++) {
        if (same_text(name, command_specs[i].name)) {
            return &command_specs[i];
        }
    }
    return NULL;
}

void pak_note_option(struct pak_options *opts, unsigned int bit, const char *token)
{
    if (opts == NULL) {
        return;
    }

    opts->option_mask |= bit;
    if (opts->seen_option_count >= PAK_MAX_SEEN_OPTIONS) {
        return;
    }

    opts->seen_options[opts->seen_option_count].bit = bit;
    opts->seen_options[opts->seen_option_count].token = token;
    opts->seen_option_count++;
}

void hint_no_command(int argc, char **argv, const struct pak_options *opts)
{
    (void)argc;
    (void)argv;
    diag_error("no command given");
    if (opts != NULL && (opts->option_mask & PAK_OPT_STORE) != 0) {
        diag_hint("--store needs repack");
        print_try_start();
        fputs(" repack", stderr);
        print_repack_options(opts);
        fputc(' ', stderr);
        print_placeholder("<archive.pak>");
        fputc('\n', stderr);
    } else if (opts != NULL && (opts->option_mask & COMPRESS_OPTIONS) != 0) {
        diag_hint("compression options need make, update, or repack");
        print_try_start();
        fputs(" repack", stderr);
        print_repack_options(opts);
        fputc(' ', stderr);
        print_placeholder("<archive.pak>");
        fputc('\n', stderr);
    } else if (opts != NULL && (opts->option_mask & WRITE_OPTIONS) != 0) {
        diag_hint("write options need make or update");
        print_try_start();
        fputs(" make", stderr);
        print_write_options(opts);
        fputc(' ', stderr);
        print_placeholder("<archive.pak>");
        fputs(" .\n", stderr);
    } else if (opts != NULL && (opts->option_mask & EXTRACT_OPTIONS) != 0) {
        diag_hint("extract options need extract or unpack");
        print_try_start();
        fputs(" unpack ", stderr);
        print_placeholder("<archive.pak>");
        print_extract_options(opts);
        fputc('\n', stderr);
    } else if (opts != NULL && (opts->option_mask & (PAK_OPT_LONG | PAK_OPT_FULL_NAME)) != 0) {
        print_try_start();
        fputs(" list", stderr);
        if (opts->full_names) {
            fputs(" --full-name", stderr);
        } else if (opts->long_list) {
            fputs(" --long", stderr);
        }
        fputc(' ', stderr);
        print_placeholder("<archive.pak>");
        fputc('\n', stderr);
    } else {
        print_try_start();
        fputs(" --help\n", stderr);
    }
}

void hint_unknown_option(const char *option, int argc, char **argv, int option_index)
{
    const struct option_spec *suggestion = closest_option(option);

    diag_error("unknown option '%s'", option);
    if (is_level_option_typo(option)) {
        if (option[2] == '1' && option[3] == '0') {
            diag_hint("compression level 10 needs --level 10");
            print_fixed_argv("--level", "10", argc, argv, option_index);
        } else {
            char short_level[3];

            short_level[0] = '-';
            short_level[1] = option[2];
            short_level[2] = '\0';
            diag_hint("compression level shortcuts use one dash: %s", short_level);
            print_fixed_argv(short_level, NULL, argc, argv, option_index);
        }
        return;
    }

    if (suggestion != NULL && suggestion_is_same_option_family(option, suggestion->name)) {
        diag_hint("did you mean '%s'?", suggestion->name);
        print_fixed_argv(suggestion->name, NULL, argc, argv, option_index);
        return;
    }

    print_try_start();
    fputs(" --help\n", stderr);
}

void hint_unknown_command(int argc, char **argv, int count, char **args, const struct pak_options *opts)
{
    const char *suggestion = closest_command(args[0]);
    const struct pak_command_spec *spec;

    (void)argc;
    (void)argv;
    diag_error("unknown command '%s'", args[0]);
    diag_known("make, update, list, extract, unpack, cat, info, delete, rename, repack, check");
    if (same_text(args[0], "verify") || same_text(args[0], "test")) {
        diag_hint("use check for archive validation");
        print_try_start();
        fputs(" check", stderr);
        print_positionals(1, count, args);
        fputc('\n', stderr);
        return;
    }
    if (suggestion == NULL) {
        return;
    }

    spec = pak_command_spec(suggestion);
    print_try_start();
    fprintf(stderr, " %s", suggestion);
    if (spec != NULL) {
        print_allowed_options(spec, opts);
    }
    print_positionals(1, count, args);
    fputc('\n', stderr);
}

void hint_bad_compression_level(const char *value)
{
    diag_error("bad compression level '%s'", value == NULL ? "" : value);
    diag_hint("levels are 0 through 10");
    print_try_start();
    fputs(" make --level 9 ", stderr);
    print_placeholder("<archive.pak>");
    fputs(" files...\n", stderr);
    diag_or_start();
    fputs("pak repack --level 9 ", stderr);
    print_placeholder("<archive.pak>");
    fputc('\n', stderr);
}

void hint_missing_option_value(const char *option, const char *value_name)
{
    diag_error("%s needs %s", option, value_name);
    print_try_start();
    fputs(" --help\n", stderr);
}

int hint_validate_command(const struct pak_command_spec *spec, int argc, char **argv, int count, char **args, const struct pak_options *opts)
{
    const struct pak_seen_option *invalid;
    int positional_count = count - 1;

    (void)argc;
    (void)argv;
    invalid = first_invalid_option(spec, opts);
    if (invalid != NULL) {
        hint_invalid_option(spec, invalid, count, args, opts);
        return -1;
    }

    if (hint_archive_arg_order(spec, count, args, opts) != 0) {
        return -1;
    }

    if (positional_count < spec->min_args) {
        hint_missing_arg(spec, count, args, opts);
        return -1;
    }
    if (spec->max_args != PAK_ARG_MANY && positional_count > spec->max_args) {
        hint_too_many_args(spec, count, args, opts);
        return -1;
    }

    return 0;
}
