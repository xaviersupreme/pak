#ifndef PAK_ARCHIVE_INTERNAL_H
#define PAK_ARCHIVE_INTERNAL_H

#include "pak.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#else
#include <sys/types.h>
#endif

#define PAK_MAGIC_1 "PAK1"
#define PAK_MAGIC_2 "PAK2"
#define PAK_FLAG_RLE 1u
#define PAK_FLAG_DEFLATE 2u
#define COPY_BUF_SIZE 65536u
#define REPAIR_COPY 0
#define REPAIR_SALVAGE 1

struct pak_entry {
    char *name;
    uint32_t flags;
    uint64_t size;
    uint64_t stored_size;
    uint32_t checksum;
};

struct old_entry_ref {
    struct pak_entry entry;
    uint64_t data_offset;
    int repair_mode;
    uint64_t available_size;
};

static int pak_file_seek(FILE *fp, uint64_t offset)
{
    if (offset > (uint64_t)INT64_MAX) {
        return -1;
    }
#ifdef _WIN32
    return _fseeki64(fp, (long long)offset, SEEK_SET);
#else
    return fseeko(fp, (off_t)offset, SEEK_SET);
#endif
}

static int pak_file_seek_end(FILE *fp)
{
#ifdef _WIN32
    return _fseeki64(fp, 0, SEEK_END);
#else
    return fseeko(fp, 0, SEEK_END);
#endif
}

static int pak_file_tell(FILE *fp, uint64_t *offset)
{
#ifdef _WIN32
    long long pos = _ftelli64(fp);
#else
    off_t pos = ftello(fp);
#endif

    if (pos < 0) {
        return -1;
    }
    *offset = (uint64_t)pos;
    return 0;
}

const char *entry_method(const struct pak_entry *entry);
void free_entry(struct pak_entry *entry);
int read_archive_header(FILE *fp, int *version, uint32_t *count);
int write_archive_header(FILE *fp, uint32_t count);
int read_entry_header(FILE *fp, int version, struct pak_entry *entry);
int write_entry_header(FILE *fp, const struct pak_entry *entry);
int copy_entry_bytes(FILE *in, FILE *out, uint64_t size, uint32_t *crc);
int copy_kept_entry(FILE *archive, FILE *out, const struct old_entry_ref *ref, int version);
void free_old_entry_refs(struct old_entry_ref *entries, uint32_t count);
int same_archive_name(const char *left, const char *right);
int process_entry_data_impl(FILE *archive, FILE *out, const struct pak_entry *entry, int version, const struct pak_options *opts, int check_crc, int report_errors);
int file_looks_binary(FILE *fp, int *binary);

#endif
