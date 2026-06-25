#ifndef PAK_H
#define PAK_H

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

struct pak_options {
    int verbose;
    int preserve_paths;
    int compress;
    int long_list;
    int overwrite_mode;
    const char *extract_dir;
};

int pak_make(const char *archive_path, int file_count, char **file_paths, const struct pak_options *opts);
int pak_list(const char *archive_path, const struct pak_options *opts);
int pak_extract(const char *archive_path, const struct pak_options *opts);
int pak_info(const char *archive_path, const struct pak_options *opts);
int pak_verify(const char *archive_path, const struct pak_options *opts);

int io_file_size(const char *path, uint64_t *size);
int io_file_exists(const char *path);
int io_make_parent_dirs(const char *path);
const char *io_base_name(const char *path);
char *io_archive_name(const char *path, int preserve_paths);
char *io_join_path(const char *dir, const char *name);
int io_is_plain_name(const char *name);
int io_is_safe_path(const char *name);

void log_step(const struct pak_options *opts, const char *fmt, ...);
void log_item(const struct pak_options *opts, int index, int total, const char *fmt, ...);
void log_progress(const struct pak_options *opts, const char *name, uint64_t done, uint64_t total, int force);

uint32_t crc32_start(void);
uint32_t crc32_update(uint32_t crc, const unsigned char *buf, size_t size);
uint32_t crc32_finish(uint32_t crc);

int rle_compress(const unsigned char *in, size_t in_size, unsigned char **out, size_t *out_size);
int rle_decompress(FILE *in, FILE *out, uint64_t in_size, uint64_t out_size, uint32_t *crc, const char *name, const struct pak_options *opts);

int read_u32_le(FILE *fp, uint32_t *value);
int read_u64_le(FILE *fp, uint64_t *value);
int write_u32_le(FILE *fp, uint32_t value);
int write_u64_le(FILE *fp, uint64_t value);

#endif