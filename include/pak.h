#ifndef PAK_H
#define PAK_H

#ifndef _WIN32
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PAK_OVERWRITE_REFUSE 0
#define PAK_OVERWRITE_REPLACE 1
#define PAK_OVERWRITE_SKIP 2
#define PAK_DEFAULT_COMPRESSION_LEVEL -1
#define PAK_ARG_MANY -1
#define PAK_MAX_SEEN_OPTIONS 64

#define PAK_CMD_MAKE 1
#define PAK_CMD_UPDATE 2
#define PAK_CMD_LIST 3
#define PAK_CMD_EXTRACT 4
#define PAK_CMD_CAT 5
#define PAK_CMD_INFO 6
#define PAK_CMD_DELETE 7
#define PAK_CMD_RENAME 8
#define PAK_CMD_CHECK 9
#define PAK_CMD_REPACK 10

#define PAK_OPT_COMPRESS 0x0001u
#define PAK_OPT_LEVEL 0x0002u
#define PAK_OPT_PATHS 0x0004u
#define PAK_OPT_EXCLUDE 0x0008u
#define PAK_OPT_NO_PAKIGNORE 0x0010u
#define PAK_OPT_LONG 0x0020u
#define PAK_OPT_C 0x0040u
#define PAK_OPT_OVERWRITE 0x0080u
#define PAK_OPT_SKIP_EXISTING 0x0100u
#define PAK_OPT_FULL_NAME 0x0200u
#define PAK_OPT_STORE 0x0400u
#define PAK_OPT_NO_SMART_COMPRESS 0x0800u

#define PAK_CLR_RESET "\033[0m"
#define PAK_CLR_BOLD "\033[1m"
#define PAK_CLR_DIM "\033[2m"
#define PAK_CLR_RED "\033[31m"
#define PAK_CLR_GREEN "\033[32m"
#define PAK_CLR_YELLOW "\033[33m"
#define PAK_CLR_CYAN "\033[36m"

struct path_list {
    char **items;
    char **names;
    int count;
    int capacity;
};

struct pak_pattern_list {
    char **items;
    int count;
    int capacity;
};

struct pak_seen_option {
    unsigned int bit;
    const char *token;
};

struct pak_options {
    int quiet;
    int preserve_paths;
    int compress;
    int smart_compress;
    int store;
    int compression_level;
    int long_list;
    int full_names;
    int overwrite_mode;
    int use_pakignore;
    const char *extract_dir;
    unsigned int option_mask;
    struct pak_seen_option seen_options[PAK_MAX_SEEN_OPTIONS];
    int seen_option_count;
    struct pak_pattern_list exclude_patterns;
};

struct pak_command_spec {
    const char *name;
    const char *canonical;
    int id;
    int min_args;
    int max_args;
    unsigned int allowed_options;
    const char *usage;
};

int pak_make(const char *archive_path, int file_count, char **file_paths, char **archive_names, const struct pak_options *opts);
int pak_update(const char *archive_path, int file_count, char **file_paths, char **archive_names, const struct pak_options *opts);
int pak_list(const char *archive_path, int selected_count, char **selected_names, const struct pak_options *opts);
int pak_extract(const char *archive_path, int selected_count, char **selected_names, const struct pak_options *opts);
int pak_cat(const char *archive_path, const char *entry_name, const struct pak_options *opts);
int pak_info(const char *archive_path, const struct pak_options *opts);
int pak_delete(const char *archive_path, int selected_count, char **selected_names, const struct pak_options *opts);
int pak_rename(const char *archive_path, const char *old_name, const char *new_name, const struct pak_options *opts);
int pak_check(const char *archive_path, const struct pak_options *opts);
int pak_repack(const char *archive_path, int selected_count, char **selected_names, const struct pak_options *opts);

int io_file_size(const char *path, uint64_t *size);
int io_file_exists(const char *path);
int io_make_parent_dirs(const char *path);
const char *io_base_name(const char *path);
char *io_archive_name(const char *path, int preserve_paths);
char *io_join_path(const char *dir, const char *name);
int io_is_plain_name(const char *name);
int io_is_safe_path(const char *name);

void path_list_init(struct path_list *list);
void path_list_free(struct path_list *list);
int path_list_add_inputs(struct path_list *list, int input_count, char **inputs, int *saw_directory);

void pattern_list_init(struct pak_pattern_list *list);
void pattern_list_free(struct pak_pattern_list *list);
int pattern_list_add(struct pak_pattern_list *list, const char *pattern);
int pattern_list_load_file(struct pak_pattern_list *list, const char *path);
int pak_pattern_has_magic(const char *pattern);
int pak_pattern_match(const char *pattern, const char *path);
int pak_is_excluded(const struct pak_options *opts, const char *archive_name);

const struct pak_command_spec *pak_command_spec(const char *name);
void pak_note_option(struct pak_options *opts, unsigned int bit, const char *token);
void hint_no_command(int argc, char **argv, const struct pak_options *opts);
void hint_unknown_option(const char *option, int argc, char **argv, int option_index);
void hint_unknown_command(int argc, char **argv, int count, char **args, const struct pak_options *opts);
void hint_bad_compression_level(const char *value);
void hint_missing_option_value(const char *option, const char *value_name);
int hint_validate_command(const struct pak_command_spec *spec, int argc, char **argv, int count, char **args, const struct pak_options *opts);

const char *pak_clr(FILE *stream, const char *code);
void diag_error(const char *fmt, ...);
void diag_hint(const char *fmt, ...);
void diag_known(const char *fmt, ...);
void diag_try(const char *fmt, ...);
void diag_or(const char *fmt, ...);
void diag_set_suppressed(int suppressed);
int diag_is_suppressed(void);
void diag_error_start(void);
void diag_hint_start(void);
void diag_known_start(void);
void diag_try_start(void);
void diag_or_start(void);
void diag_placeholder(const char *text);
void log_step(const struct pak_options *opts, const char *fmt, ...);
void log_item(const struct pak_options *opts, int index, int total, const char *fmt, ...);
void log_finish_progress(void);
void log_progress(const struct pak_options *opts, const char *name, uint64_t done, uint64_t total, int force);
void log_count_progress(const struct pak_options *opts, const char *name, uint64_t done, uint64_t total, int force);

uint32_t crc32_start(void);
uint32_t crc32_update(uint32_t crc, const unsigned char *buf, size_t size);
uint32_t crc32_finish(uint32_t crc);

int rle_compress(const unsigned char *in, size_t in_size, unsigned char **out, size_t *out_size);
int rle_decompress(FILE *in, FILE *out, uint64_t in_size, uint64_t out_size, uint32_t *crc, const char *name, const struct pak_options *opts);
int rle_recover(FILE *in, FILE *out, uint64_t in_size, uint64_t out_size, uint32_t *crc, uint64_t *wrote, const char *name, const struct pak_options *opts);
int deflate_compress(const unsigned char *in, size_t in_size, int level, unsigned char **out, size_t *out_size);
int deflate_decompress(FILE *in, FILE *out, uint64_t in_size, uint64_t out_size, uint32_t *crc, const char *name, const struct pak_options *opts);
int deflate_recover(FILE *in, FILE *out, uint64_t in_size, uint64_t out_size, uint32_t *crc, uint64_t *wrote, const char *name, const struct pak_options *opts);

int read_u32_le(FILE *fp, uint32_t *value);
int read_u64_le(FILE *fp, uint64_t *value);
int write_u32_le(FILE *fp, uint32_t value);
int write_u64_le(FILE *fp, uint64_t value);

#endif
