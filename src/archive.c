#include "pak.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define stdout_is_tty() _isatty(_fileno(stdout))
#else
#include <unistd.h>
#define stdout_is_tty() isatty(fileno(stdout))
#endif

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PAK_MAGIC_1 "PAK1"
#define PAK_MAGIC_2 "PAK2"
#define PAK_FLAG_COMPRESSED 1u
#define COPY_BUF_SIZE 65536u

struct pak_entry {
    char *name;
    uint32_t flags;
    uint64_t size;
    uint64_t stored_size;
    uint32_t checksum;
};

static int read_exact(FILE *fp, void *buf, size_t size)
{
    return fread(buf, 1, size, fp) == size ? 0 : -1;
}

static int write_exact(FILE *fp, const void *buf, size_t size)
{
    return fwrite(buf, 1, size, fp) == size ? 0 : -1;
}

static const char *entry_method(const struct pak_entry *entry)
{
    return (entry->flags & PAK_FLAG_COMPRESSED) != 0 ? "rle" : "store";
}

static void free_entry(struct pak_entry *entry)
{
    free(entry->name);
    memset(entry, 0, sizeof(*entry));
}

static int read_archive_header(FILE *fp, int *version, uint32_t *count)
{
    char magic[4];

    if (read_exact(fp, magic, sizeof(magic)) != 0) {
        return -1;
    }
    if (memcmp(magic, PAK_MAGIC_1, sizeof(magic)) == 0) {
        *version = 1;
    } else if (memcmp(magic, PAK_MAGIC_2, sizeof(magic)) == 0) {
        *version = 2;
    } else {
        return -1;
    }

    return read_u32_le(fp, count);
}

static int write_archive_header(FILE *fp, uint32_t count)
{
    if (write_exact(fp, PAK_MAGIC_2, 4) != 0) {
        return -1;
    }
    return write_u32_le(fp, count);
}

static int read_name(FILE *fp, uint32_t name_len, char **name)
{
    char *tmp;

    if (name_len == 0 || name_len > PATH_MAX) {
        return -1;
    }

    tmp = malloc((size_t)name_len + 1);
    if (tmp == NULL) {
        return -1;
    }
    if (read_exact(fp, tmp, name_len) != 0) {
        free(tmp);
        return -1;
    }
    tmp[name_len] = '\0';

    *name = tmp;
    return 0;
}

static int read_entry_header(FILE *fp, int version, struct pak_entry *entry)
{
    uint32_t name_len;

    memset(entry, 0, sizeof(*entry));

    if (read_u32_le(fp, &name_len) != 0) {
        return -1;
    }

    if (version == 1) {
        if (read_u64_le(fp, &entry->size) != 0 || read_name(fp, name_len, &entry->name) != 0) {
            return -1;
        }
        entry->stored_size = entry->size;
        entry->flags = 0;
        entry->checksum = 0;
        return io_is_plain_name(entry->name) ? 0 : -1;
    }

    if (read_u32_le(fp, &entry->flags) != 0 || read_u64_le(fp, &entry->size) != 0 || read_u64_le(fp, &entry->stored_size) != 0 || read_u32_le(fp, &entry->checksum) != 0 || read_name(fp, name_len, &entry->name) != 0) {
        return -1;
    }
    if ((entry->flags & ~PAK_FLAG_COMPRESSED) != 0) {
        return -1;
    }

    return io_is_safe_path(entry->name) ? 0 : -1;
}

static int write_entry_header(FILE *fp, const struct pak_entry *entry)
{
    size_t name_len = strlen(entry->name);

    if (name_len == 0 || name_len > UINT32_MAX) {
        return -1;
    }
    if (write_u32_le(fp, (uint32_t)name_len) != 0 || write_u32_le(fp, entry->flags) != 0 || write_u64_le(fp, entry->size) != 0 || write_u64_le(fp, entry->stored_size) != 0 || write_u32_le(fp, entry->checksum) != 0) {
        return -1;
    }

    return write_exact(fp, entry->name, name_len);
}

static int skip_bytes(FILE *fp, uint64_t size)
{
    unsigned char buf[COPY_BUF_SIZE];
    uint64_t left = size;

    while (left > 0) {
        size_t want = left > COPY_BUF_SIZE ? COPY_BUF_SIZE : (size_t)left;
        if (fread(buf, 1, want, fp) != want) {
            return -1;
        }
        left -= want;
    }

    return 0;
}

static int copy_stored_data(FILE *in, FILE *out, uint64_t size, uint32_t *crc, const char *name, const struct pak_options *opts)
{
    unsigned char buf[COPY_BUF_SIZE];
    uint64_t left = size;
    uint64_t done = 0;

    while (left > 0) {
        size_t want = left > COPY_BUF_SIZE ? COPY_BUF_SIZE : (size_t)left;
        size_t got = fread(buf, 1, want, in);

        if (got == 0) {
            return -1;
        }
        if (out != NULL && fwrite(buf, 1, got, out) != got) {
            return -1;
        }

        *crc = crc32_update(*crc, buf, got);
        done += got;
        left -= got;
        log_progress(opts, name, done, size, left == 0);
    }

    if (size == 0) {
        log_progress(opts, name, 0, 0, 1);
    }

    return 0;
}

static int read_whole_file(const char *path, const char *name, unsigned char **data, size_t *size, uint32_t *checksum, const struct pak_options *opts)
{
    FILE *fp;
    uint64_t file_size;
    uint64_t done;
    uint32_t crc;

    if (io_file_size(path, &file_size) != 0) {
        return -1;
    }
    if (file_size > SIZE_MAX) {
        errno = EFBIG;
        return -1;
    }

    *data = malloc(file_size == 0 ? 1 : (size_t)file_size);
    if (*data == NULL) {
        errno = ENOMEM;
        return -1;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        free(*data);
        *data = NULL;
        return -1;
    }

    done = 0;
    crc = crc32_start();
    while (done < file_size) {
        size_t want = file_size - done > COPY_BUF_SIZE ? COPY_BUF_SIZE : (size_t)(file_size - done);
        size_t got = fread(*data + done, 1, want, fp);

        if (got == 0) {
            fclose(fp);
            free(*data);
            *data = NULL;
            return -1;
        }
        crc = crc32_update(crc, *data + done, got);
        done += got;
        log_progress(opts, name, done, file_size, done == file_size);
    }

    fclose(fp);
    *size = (size_t)file_size;
    *checksum = crc32_finish(crc);
    if (file_size == 0) {
        log_progress(opts, name, 0, 0, 1);
    }
    return 0;
}

static int same_archive_name(const char *left, const char *right)
{
#ifdef _WIN32
    while (*left != '\0' && *right != '\0') {
        char a = *left;
        char b = *right;

        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
        left++;
        right++;
    }
    return *left == *right;
#else
    return strcmp(left, right) == 0;
#endif
}

static char **build_archive_names(int file_count, char **file_paths, const struct pak_options *opts)
{
    char **names;
    int i;
    int j;

    names = calloc((size_t)file_count, sizeof(*names));
    if (names == NULL) {
        return NULL;
    }

    for (i = 0; i < file_count; i++) {
        names[i] = io_archive_name(file_paths[i], opts->preserve_paths);
        if (names[i] == NULL) {
            fprintf(stderr, "pak: bad archive path '%s'\n", file_paths[i]);
            goto fail;
        }
        for (j = 0; j < i; j++) {
            if (same_archive_name(names[i], names[j])) {
                fprintf(stderr, "pak: duplicate archive name '%s'\n", names[i]);
                goto fail;
            }
        }
    }

    return names;

fail:
    for (i = 0; i < file_count; i++) {
        free(names[i]);
    }
    free(names);
    return NULL;
}

static void free_archive_names(char **names, int count)
{
    int i;

    if (names == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

static int write_entry_data(FILE *archive, const struct pak_entry *entry, const unsigned char *stored_data)
{
    if (write_entry_header(archive, entry) != 0) {
        return -1;
    }
    return write_exact(archive, stored_data, (size_t)entry->stored_size);
}

int pak_make(const char *archive_path, int file_count, char **file_paths, const struct pak_options *opts)
{
    FILE *archive;
    char **names;
    int i;

    if (file_count <= 0) {
        fprintf(stderr, "pak: no input files\n");
        return -1;
    }

    names = build_archive_names(file_count, file_paths, opts);
    if (names == NULL) {
        return -1;
    }

    log_step(opts, "create %s", archive_path);
    archive = fopen(archive_path, "wb");
    if (archive == NULL) {
        fprintf(stderr, "pak: %s: %s\n", archive_path, strerror(errno));
        free_archive_names(names, file_count);
        return -1;
    }

    if (write_archive_header(archive, (uint32_t)file_count) != 0) {
        fprintf(stderr, "pak: failed to write archive header\n");
        fclose(archive);
        free_archive_names(names, file_count);
        return -1;
    }

    log_step(opts, "pack %d file%s", file_count, file_count == 1 ? "" : "s");
    for (i = 0; i < file_count; i++) {
        struct pak_entry entry;
        unsigned char *raw_data;
        unsigned char *packed_data;
        unsigned char *stored_data;
        size_t raw_size;
        size_t packed_size;

        memset(&entry, 0, sizeof(entry));
        raw_data = NULL;
        packed_data = NULL;
        stored_data = NULL;
        raw_size = 0;
        packed_size = 0;

        log_item(opts, i + 1, file_count, "pack %s", names[i]);
        if (read_whole_file(file_paths[i], names[i], &raw_data, &raw_size, &entry.checksum, opts) != 0) {
            fprintf(stderr, "pak: %s: %s\n", file_paths[i], strerror(errno));
            fclose(archive);
            free_archive_names(names, file_count);
            return -1;
        }

        stored_data = raw_data;
        entry.name = names[i];
        entry.size = (uint64_t)raw_size;
        entry.stored_size = (uint64_t)raw_size;
        entry.flags = 0;

        if (opts->compress && raw_size > 0 && rle_compress(raw_data, raw_size, &packed_data, &packed_size) == 0 && packed_size < raw_size) {
            stored_data = packed_data;
            entry.stored_size = (uint64_t)packed_size;
            entry.flags = PAK_FLAG_COMPRESSED;
            log_step(opts, "compressed %s: %llu -> %llu bytes", names[i], (unsigned long long)entry.size, (unsigned long long)entry.stored_size);
        }

        if (write_entry_data(archive, &entry, stored_data) != 0) {
            fprintf(stderr, "pak: failed while packing '%s'\n", file_paths[i]);
            free(raw_data);
            free(packed_data);
            fclose(archive);
            free_archive_names(names, file_count);
            return -1;
        }

        free(raw_data);
        free(packed_data);
    }

    if (fclose(archive) != 0) {
        fprintf(stderr, "pak: failed to finish %s\n", archive_path);
        free_archive_names(names, file_count);
        return -1;
    }

    log_step(opts, "done");
    free_archive_names(names, file_count);
    return 0;
}

static int decimal_width_u64(uint64_t value)
{
    int width = 1;

    while (value >= 10) {
        value /= 10;
        width++;
    }

    return width;
}


static int max_int(int left, int right)
{
    return left > right ? left : right;
}

static int entry_is_selected(const char *entry_name, int selected_count, char **selected_names)
{
    int i;

    if (selected_count == 0) {
        return 1;
    }

    for (i = 0; i < selected_count; i++) {
        if (strcmp(entry_name, selected_names[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static void mark_selected(const char *entry_name, int selected_count, char **selected_names, int *seen)
{
    int i;

    for (i = 0; i < selected_count; i++) {
        if (strcmp(entry_name, selected_names[i]) == 0) {
            seen[i] = 1;
        }
    }
}

static int report_missing_selected(int selected_count, char **selected_names, int *seen)
{
    int missing = 0;
    int i;

    for (i = 0; i < selected_count; i++) {
        if (!seen[i]) {
            fprintf(stderr, "pak: not found '%s'\n", selected_names[i]);
            missing = 1;
        }
    }

    return missing ? -1 : 0;
}

int pak_list(const char *archive_path, const struct pak_options *opts)
{
    FILE *archive;
    struct pak_entry *entries;
    uint32_t count;
    uint32_t i;
    int version;
    int name_width;
    int size_width;
    int stored_width;

    log_step(opts, "read %s", archive_path);
    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        fprintf(stderr, "pak: %s: %s\n", archive_path, strerror(errno));
        return -1;
    }

    if (read_archive_header(archive, &version, &count) != 0) {
        fprintf(stderr, "pak: bad archive '%s'\n", archive_path);
        fclose(archive);
        return -1;
    }

    entries = calloc(count == 0 ? 1 : count, sizeof(*entries));
    if (entries == NULL) {
        fprintf(stderr, "pak: out of memory\n");
        fclose(archive);
        return -1;
    }

    name_width = 4;
    size_width = 4;
    stored_width = 6;
    for (i = 0; i < count; i++) {
        if (read_entry_header(archive, version, &entries[i]) != 0) {
            fprintf(stderr, "pak: damaged entry in '%s'\n", archive_path);
            goto fail;
        }

        name_width = max_int(name_width, (int)strlen(entries[i].name));
        size_width = max_int(size_width, decimal_width_u64(entries[i].size));
        stored_width = max_int(stored_width, decimal_width_u64(entries[i].stored_size));

        if (skip_bytes(archive, entries[i].stored_size) != 0) {
            fprintf(stderr, "pak: damaged data for '%s'\n", entries[i].name);
            goto fail;
        }
    }

    if (opts->long_list) {
        printf("%-*s  %*s  %*s  %-6s  %-8s\n", name_width, "name", size_width, "size", stored_width, "stored", "method", "crc32");
        for (i = 0; i < count; i++) {
            if (version == 1) {
                printf("%-*s  %*llu  %*llu  %-6s  %-8s\n", name_width, entries[i].name, size_width, (unsigned long long)entries[i].size, stored_width, (unsigned long long)entries[i].stored_size, entry_method(&entries[i]), "-");
            } else {
                printf("%-*s  %*llu  %*llu  %-6s  %08x\n", name_width, entries[i].name, size_width, (unsigned long long)entries[i].size, stored_width, (unsigned long long)entries[i].stored_size, entry_method(&entries[i]), entries[i].checksum);
            }
        }
    } else {
        for (i = 0; i < count; i++) {
            printf("%-*s  %llu bytes\n", name_width, entries[i].name, (unsigned long long)entries[i].size);
        }
    }

    for (i = 0; i < count; i++) {
        free_entry(&entries[i]);
    }
    free(entries);
    fclose(archive);
    return 0;

fail:
    for (i = 0; i < count; i++) {
        free_entry(&entries[i]);
    }
    free(entries);
    fclose(archive);
    return -1;
}
static int process_entry_data(FILE *archive, FILE *out, const struct pak_entry *entry, int version, const struct pak_options *opts, int check_crc)
{
    uint32_t crc = crc32_start();
    uint32_t actual;

    if ((entry->flags & PAK_FLAG_COMPRESSED) != 0) {
        if (rle_decompress(archive, out, entry->stored_size, entry->size, &crc, entry->name, opts) != 0) {
            return -1;
        }
    } else {
        if (copy_stored_data(archive, out, entry->stored_size, &crc, entry->name, opts) != 0) {
            return -1;
        }
    }

    actual = crc32_finish(crc);
    if (check_crc && version >= 2 && actual != entry->checksum) {
        fprintf(stderr, "pak: checksum mismatch for '%s'\n", entry->name);
        return -1;
    }

    return 0;
}

int pak_cat(const char *archive_path, const char *entry_name, const struct pak_options *opts)
{
    FILE *archive;
    struct pak_options quiet_opts;
    uint32_t count;
    uint32_t i;
    int version;
    int found = 0;

    quiet_opts = *opts;
    quiet_opts.quiet = 1;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        fprintf(stderr, "pak: %s: %s\n", archive_path, strerror(errno));
        return -1;
    }

    if (read_archive_header(archive, &version, &count) != 0) {
        fprintf(stderr, "pak: bad archive '%s'\n", archive_path);
        fclose(archive);
        return -1;
    }

    for (i = 0; i < count; i++) {
        struct pak_entry entry;

        if (read_entry_header(archive, version, &entry) != 0) {
            fprintf(stderr, "pak: damaged entry in '%s'\n", archive_path);
            fclose(archive);
            return -1;
        }

        if (strcmp(entry.name, entry_name) != 0) {
            if (skip_bytes(archive, entry.stored_size) != 0) {
                fprintf(stderr, "pak: damaged data for '%s'\n", entry.name);
                free_entry(&entry);
                fclose(archive);
                return -1;
            }
            free_entry(&entry);
            continue;
        }

        found = 1;
        if (stdout_is_tty() && entry.size > 1024ull * 1024ull) {
            fprintf(stderr, "pak: '%s' is %llu bytes; cat would dump it into your terminal\n", entry.name, (unsigned long long)entry.size);
            fprintf(stderr, "try: pak cat %s %s > %s\n", archive_path, entry.name, entry.name);
            fprintf(stderr, "or:  pak unpack %s %s\n", archive_path, entry.name);
            free_entry(&entry);
            fclose(archive);
            return -1;
        }
        if (process_entry_data(archive, stdout, &entry, version, &quiet_opts, 1) != 0) {
            fprintf(stderr, "pak: failed while reading '%s'\n", entry.name);
            free_entry(&entry);
            fclose(archive);
            return -1;
        }
        fflush(stdout);
        free_entry(&entry);
        break;
    }

    fclose(archive);
    if (!found) {
        fprintf(stderr, "pak: not found '%s'\n", entry_name);
        return -1;
    }

    return 0;
}

int pak_extract(const char *archive_path, int selected_count, char **selected_names, const struct pak_options *opts)
{
    FILE *archive;
    int *seen;
    uint32_t count;
    uint32_t i;
    int version;

    seen = calloc(selected_count == 0 ? 1 : (size_t)selected_count, sizeof(*seen));
    if (seen == NULL) {
        fprintf(stderr, "pak: out of memory\n");
        return -1;
    }

    log_step(opts, "read %s", archive_path);
    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        fprintf(stderr, "pak: %s: %s\n", archive_path, strerror(errno));
        free(seen);
        return -1;
    }

    if (read_archive_header(archive, &version, &count) != 0) {
        fprintf(stderr, "pak: bad archive '%s'\n", archive_path);
        fclose(archive);
        free(seen);
        return -1;
    }

    if (selected_count == 0) {
        log_step(opts, "extract %u file%s", count, count == 1 ? "" : "s");
    } else {
        log_step(opts, "extract %d selected file%s", selected_count, selected_count == 1 ? "" : "s");
    }
    for (i = 0; i < count; i++) {
        struct pak_entry entry;
        char *out_path;
        FILE *out;

        if (read_entry_header(archive, version, &entry) != 0) {
            fprintf(stderr, "pak: damaged entry in '%s'\n", archive_path);
            fclose(archive);
            free(seen);
            return -1;
        }

        if (!entry_is_selected(entry.name, selected_count, selected_names)) {
            if (skip_bytes(archive, entry.stored_size) != 0) {
                fprintf(stderr, "pak: damaged data for '%s'\n", entry.name);
                free_entry(&entry);
                fclose(archive);
                free(seen);
                return -1;
            }
            free_entry(&entry);
            continue;
        }
        mark_selected(entry.name, selected_count, selected_names, seen);

        out_path = io_join_path(opts->extract_dir, entry.name);
        if (out_path == NULL) {
            fprintf(stderr, "pak: out of memory\n");
            free_entry(&entry);
            fclose(archive);
            free(seen);
            return -1;
        }

        if (io_file_exists(out_path)) {
            if (opts->overwrite_mode == PAK_OVERWRITE_SKIP) {
                log_item(opts, (int)i + 1, (int)count, "skip %s", entry.name);
                if (skip_bytes(archive, entry.stored_size) != 0) {
                    fprintf(stderr, "pak: damaged data for '%s'\n", entry.name);
                    free(out_path);
                    free_entry(&entry);
                    fclose(archive);
                    free(seen);
                    return -1;
                }
                free(out_path);
                free_entry(&entry);
                continue;
            }
            if (opts->overwrite_mode != PAK_OVERWRITE_REPLACE) {
                fprintf(stderr, "pak: refusing to overwrite '%s'\n", out_path);
                free(out_path);
                free_entry(&entry);
                fclose(archive);
                free(seen);
                return -1;
            }
        }

        if (io_make_parent_dirs(out_path) != 0) {
            fprintf(stderr, "pak: cannot create parent directory for '%s'\n", out_path);
            free(out_path);
            free_entry(&entry);
            fclose(archive);
            free(seen);
            return -1;
        }

        log_item(opts, (int)i + 1, (int)count, "extract %s", entry.name);
        out = fopen(out_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "pak: %s: %s\n", out_path, strerror(errno));
            free(out_path);
            free_entry(&entry);
            fclose(archive);
            free(seen);
            return -1;
        }

        if (process_entry_data(archive, out, &entry, version, opts, 1) != 0) {
            fprintf(stderr, "pak: failed while extracting '%s'\n", entry.name);
            fclose(out);
            free(out_path);
            free_entry(&entry);
            fclose(archive);
            free(seen);
            return -1;
        }

        fclose(out);
        free(out_path);
        free_entry(&entry);
    }

    fclose(archive);
    log_step(opts, "done");
    if (report_missing_selected(selected_count, selected_names, seen) != 0) {
        free(seen);
        return -1;
    }
    free(seen);
    return 0;
}

int pak_info(const char *archive_path, const struct pak_options *opts)
{
    FILE *archive;
    uint32_t count;
    uint32_t i;
    uint64_t archive_size;
    uint64_t unpacked_size = 0;
    uint64_t stored_size = 0;
    uint32_t compressed = 0;
    int version;

    (void)opts;
    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        fprintf(stderr, "pak: %s: %s\n", archive_path, strerror(errno));
        return -1;
    }
    if (read_archive_header(archive, &version, &count) != 0 || io_file_size(archive_path, &archive_size) != 0) {
        fprintf(stderr, "pak: bad archive '%s'\n", archive_path);
        fclose(archive);
        return -1;
    }

    for (i = 0; i < count; i++) {
        struct pak_entry entry;

        if (read_entry_header(archive, version, &entry) != 0) {
            fprintf(stderr, "pak: damaged entry in '%s'\n", archive_path);
            fclose(archive);
            return -1;
        }
        unpacked_size += entry.size;
        stored_size += entry.stored_size;
        if ((entry.flags & PAK_FLAG_COMPRESSED) != 0) {
            compressed++;
        }
        if (skip_bytes(archive, entry.stored_size) != 0) {
            fprintf(stderr, "pak: damaged data for '%s'\n", entry.name);
            free_entry(&entry);
            fclose(archive);
            return -1;
        }
        free_entry(&entry);
    }

    printf("archive: %s\n", archive_path);
    printf("format: PAK%d\n", version);
    printf("files: %u\n", count);
    printf("archive size: %llu bytes\n", (unsigned long long)archive_size);
    printf("unpacked size: %llu bytes\n", (unsigned long long)unpacked_size);
    printf("stored size: %llu bytes\n", (unsigned long long)stored_size);
    printf("compressed files: %u\n", compressed);
    if (unpacked_size > 0) {
        printf("stored ratio: %.1f%%\n", (double)stored_size * 100.0 / (double)unpacked_size);
    }

    fclose(archive);
    return 0;
}

int pak_verify(const char *archive_path, const struct pak_options *opts)
{
    FILE *archive;
    uint32_t count;
    uint32_t i;
    int version;

    log_step(opts, "verify %s", archive_path);
    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        fprintf(stderr, "pak: %s: %s\n", archive_path, strerror(errno));
        return -1;
    }
    if (read_archive_header(archive, &version, &count) != 0) {
        fprintf(stderr, "pak: bad archive '%s'\n", archive_path);
        fclose(archive);
        return -1;
    }

    for (i = 0; i < count; i++) {
        struct pak_entry entry;

        if (read_entry_header(archive, version, &entry) != 0) {
            fprintf(stderr, "pak: damaged entry in '%s'\n", archive_path);
            fclose(archive);
            return -1;
        }
        log_item(opts, (int)i + 1, (int)count, "check %s", entry.name);
        if (process_entry_data(archive, NULL, &entry, version, opts, version >= 2) != 0) {
            free_entry(&entry);
            fclose(archive);
            return -1;
        }
        free_entry(&entry);
    }

    fclose(archive);
    printf("ok: %u file%s verified\n", count, count == 1 ? "" : "s");
    return 0;
}
