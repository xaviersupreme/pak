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
#include <windows.h>
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
#define PAK_FLAG_RLE 1u
#define PAK_FLAG_DEFLATE 2u
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

static const char *out_clr(const char *code)
{
    return pak_clr(stdout, code);
}

static int shell_safe_arg_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '/' || ch == '\\' || ch == ':' || ch == '=';
}

static void print_shell_arg(FILE *out, const char *arg)
{
    const char *p;

    if (arg != NULL && arg[0] != '\0') {
        for (p = arg; *p != '\0'; p++) {
            if (!shell_safe_arg_char(*p)) {
                break;
            }
        }
        if (*p == '\0') {
            fputs(arg, out);
            return;
        }
    }

    fputc('"', out);
    if (arg != NULL) {
        for (p = arg; *p != '\0'; p++) {
            if (*p == '"' || *p == '\\') {
                fputc('\\', out);
            }
            fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void print_cat_redirect_hint(const char *archive_path, const char *entry_name)
{
    if (diag_is_suppressed()) {
        return;
    }

    diag_try_start();
    fputs("pak cat ", stderr);
    print_shell_arg(stderr, archive_path);
    fputc(' ', stderr);
    print_shell_arg(stderr, entry_name);
    fputs(" > ", stderr);
    print_shell_arg(stderr, entry_name);
    fputc('\n', stderr);
}

static void print_unpack_hint(const char *archive_path, const char *entry_name)
{
    if (diag_is_suppressed()) {
        return;
    }

    diag_or_start();
    fputs("pak unpack ", stderr);
    print_shell_arg(stderr, archive_path);
    fputc(' ', stderr);
    print_shell_arg(stderr, entry_name);
    fputc('\n', stderr);
}

static void print_list_hint(const char *archive_path, const char *entry_name)
{
    if (diag_is_suppressed()) {
        return;
    }

    diag_try_start();
    fputs("pak list ", stderr);
    print_shell_arg(stderr, archive_path);
    fputc(' ', stderr);
    print_shell_arg(stderr, entry_name);
    fputc('\n', stderr);
}

static void print_command_entry_hint(const char *command, const char *archive_path, const char *entry_name)
{
    if (diag_is_suppressed()) {
        return;
    }

    diag_try_start();
    fputs("pak ", stderr);
    fputs(command, stderr);
    fputc(' ', stderr);
    print_shell_arg(stderr, archive_path);
    fputc(' ', stderr);
    print_shell_arg(stderr, entry_name);
    fputc('\n', stderr);
}

static void print_rename_entry_hint(const char *archive_path, const char *old_name, const char *new_name)
{
    if (diag_is_suppressed()) {
        return;
    }

    diag_try_start();
    fputs("pak rename ", stderr);
    print_shell_arg(stderr, archive_path);
    fputc(' ', stderr);
    print_shell_arg(stderr, old_name);
    fputc(' ', stderr);
    print_shell_arg(stderr, new_name);
    fputc('\n', stderr);
}

static const char *entry_base_name(const char *name)
{
    const char *base = name;

    while (*name != '\0') {
        if (*name == '/' || *name == '\\') {
            base = name + 1;
        }
        name++;
    }
    return base;
}

static int archive_min3(int a, int b, int c)
{
    int min = a < b ? a : b;

    return min < c ? min : c;
}

static int archive_edit_distance(const char *left, const char *right)
{
    int prev[128];
    int curr[128];
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    size_t i;
    size_t j;

    if (left_len >= 128 || right_len >= 128) {
        return 999;
    }

    for (j = 0; j <= right_len; j++) {
        prev[j] = (int)j;
    }

    for (i = 1; i <= left_len; i++) {
        curr[0] = (int)i;
        for (j = 1; j <= right_len; j++) {
            int cost = left[i - 1] == right[j - 1] ? 0 : 1;

            curr[j] = archive_min3(prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost);
        }
        for (j = 0; j <= right_len; j++) {
            prev[j] = curr[j];
        }
    }

    return prev[right_len];
}

static void print_list_search_hint(const char *archive_path, const char *query)
{
    const char *base = entry_base_name(query);
    char *pattern;
    size_t len;

    len = strlen(base);
    pattern = malloc(len + 2);
    if (pattern == NULL) {
        print_list_hint(archive_path, query);
        return;
    }

    pattern[0] = '*';
    memcpy(pattern + 1, base, len + 1);
    print_list_hint(archive_path, pattern);
    free(pattern);
}

static const char *entry_method(const struct pak_entry *entry)
{
    if ((entry->flags & PAK_FLAG_DEFLATE) != 0) {
        return "deflate";
    }
    if ((entry->flags & PAK_FLAG_RLE) != 0) {
        return "rle";
    }
    return "store";
}

static void free_entry(struct pak_entry *entry)
{
    free(entry->name);
    memset(entry, 0, sizeof(*entry));
}

static int read_archive_header(FILE *fp, int *version, uint32_t *count)
{
    char magic[4];
    long pos;
    long end;
    long remaining;
    long min_entry_size;

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

    if (read_u32_le(fp, count) != 0) {
        return -1;
    }

    pos = ftell(fp);
    if (pos < 0 || fseek(fp, 0, SEEK_END) != 0) {
        return -1;
    }
    end = ftell(fp);
    if (end < 0 || fseek(fp, pos, SEEK_SET) != 0) {
        return -1;
    }

    min_entry_size = *version == 1 ? 13 : 29;
    remaining = end - pos;
    if (remaining < 0 || *count > (uint32_t)(remaining / min_entry_size)) {
        return -1;
    }

    return 0;
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
    if (entry->flags != 0 && entry->flags != PAK_FLAG_RLE && entry->flags != PAK_FLAG_DEFLATE) {
        return -1;
    }
    if (entry->flags == 0 && entry->stored_size != entry->size) {
        return -1;
    }
    if (entry->flags != 0 && entry->stored_size > entry->size) {
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

struct make_input {
    char *path;
    char *name;
};

struct old_entry_ref {
    struct pak_entry entry;
    long data_offset;
};

static const char *closest_entry_name(struct old_entry_ref *entries, uint32_t count, const char *query)
{
    const char *query_base = entry_base_name(query);
    const char *best = NULL;
    int best_score = 999;
    uint32_t i;

    for (i = 0; i < count; i++) {
        const char *entry_base = entry_base_name(entries[i].entry.name);
        int score;

        if (strcmp(query_base, entry_base) == 0) {
            return entries[i].entry.name;
        }

        score = archive_edit_distance(query, entries[i].entry.name);
        if (score < best_score) {
            best_score = score;
            best = entries[i].entry.name;
        }
    }

    if (best_score <= 3 || best_score <= (int)strlen(query) / 3) {
        return best;
    }
    return NULL;
}

static void report_missing_entry_with_hints(const char *command, const char *archive_path, const char *query, struct old_entry_ref *entries, uint32_t count)
{
    const char *suggestion;

    if (pak_pattern_has_magic(query)) {
        diag_error("no match for '%s'", query);
        print_list_hint(archive_path, query);
        return;
    }

    diag_error("not found '%s'", query);
    suggestion = closest_entry_name(entries, count, query);
    if (suggestion != NULL) {
        diag_hint("closest entry is '%s'", suggestion);
        print_command_entry_hint(command, archive_path, suggestion);
        return;
    }

    diag_hint("check the current archive names");
    print_list_search_hint(archive_path, query);
}

static void report_missing_rename_source(const char *archive_path, const char *old_name, const char *new_name, struct old_entry_ref *entries, uint32_t count)
{
    const char *suggestion;

    if (pak_pattern_has_magic(old_name)) {
        diag_error("no match for '%s'", old_name);
        print_list_hint(archive_path, old_name);
        return;
    }

    diag_error("not found '%s'", old_name);
    suggestion = closest_entry_name(entries, count, old_name);
    if (suggestion != NULL) {
        diag_hint("closest entry is '%s'", suggestion);
        print_rename_entry_hint(archive_path, suggestion, new_name);
        return;
    }

    diag_hint("check the current archive names");
    print_list_search_hint(archive_path, old_name);
}

static char *clean_path_copy(const char *path)
{
    char *out;
    char *w;
    const char *r;

    out = malloc(strlen(path) + 1);
    if (out == NULL) {
        return NULL;
    }

    for (w = out, r = path; *r != '\0'; r++) {
        char ch = *r == '\\' ? '/' : *r;

        if (ch == '/' && w > out && w[-1] == '/') {
            continue;
        }
        *w++ = ch;
    }
    *w = '\0';

    while (out[0] == '.' && out[1] == '/') {
        memmove(out, out + 2, strlen(out + 2) + 1);
    }
    return out;
}

static int same_input_path(const char *left, const char *right)
{
    char *a;
    char *b;
    int same;

    a = clean_path_copy(left);
    b = clean_path_copy(right);
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return 0;
    }

    same = same_archive_name(a, b);
    free(a);
    free(b);
    return same;
}

static int compare_inputs_by_name(const void *left, const void *right)
{
    const struct make_input *a = left;
    const struct make_input *b = right;
    int by_name = strcmp(a->name, b->name);

    if (by_name != 0) {
        return by_name;
    }
    return strcmp(a->path, b->path);
}

static int compare_old_entries_by_name(const void *left, const void *right)
{
    const struct old_entry_ref *a = left;
    const struct old_entry_ref *b = right;

    return strcmp(a->entry.name, b->entry.name);
}

static void free_make_inputs(struct make_input *inputs, int count)
{
    int i;

    if (inputs == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(inputs[i].name);
    }
    free(inputs);
}

static void free_old_entry_refs(struct old_entry_ref *entries, uint32_t count)
{
    uint32_t i;

    if (entries == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        free_entry(&entries[i].entry);
    }
    free(entries);
}

static char *archive_string_copy(const char *value)
{
    char *out = malloc(strlen(value) + 1);

    if (out != NULL) {
        strcpy(out, value);
    }
    return out;
}

static int build_make_inputs(const char *archive_path, int file_count, char **file_paths, const struct pak_options *opts, struct make_input **out_inputs, int *out_count)
{
    struct make_input *inputs;
    int count;
    int i;

    *out_inputs = NULL;
    *out_count = 0;
    inputs = calloc(file_count == 0 ? 1 : (size_t)file_count, sizeof(*inputs));
    if (inputs == NULL) {
        diag_error("out of memory");
        return -1;
    }

    count = 0;
    for (i = 0; i < file_count; i++) {
        char *name;

        if (same_input_path(file_paths[i], archive_path)) {
            log_step(opts, "skip output archive %s", file_paths[i]);
            continue;
        }

        name = io_archive_name(file_paths[i], opts->preserve_paths);
        if (name == NULL) {
            diag_error("bad archive path '%s'", file_paths[i]);
            free_make_inputs(inputs, count);
            return -1;
        }
        if (pak_is_excluded(opts, name)) {
            log_step(opts, "skip ignored %s", name);
            free(name);
            continue;
        }

        inputs[count].path = file_paths[i];
        inputs[count].name = name;
        count++;
    }

    if (count > 1) {
        qsort(inputs, (size_t)count, sizeof(*inputs), compare_inputs_by_name);
        for (i = 1; i < count; i++) {
            if (same_archive_name(inputs[i - 1].name, inputs[i].name)) {
                diag_error("duplicate archive name '%s'", inputs[i].name);
                free_make_inputs(inputs, count);
                return -1;
            }
        }
    }

    *out_inputs = inputs;
    *out_count = count;
    return 0;
}

static int write_entry_data(FILE *archive, const struct pak_entry *entry, const unsigned char *stored_data)
{
    if (write_entry_header(archive, entry) != 0) {
        return -1;
    }
    return write_exact(archive, stored_data, (size_t)entry->stored_size);
}

static int pack_file_entry(FILE *archive, const struct make_input *input, int index, int total, const struct pak_options *opts)
{
    struct pak_entry entry;
    unsigned char *raw_data;
    unsigned char *deflate_data;
    unsigned char *rle_data;
    unsigned char *stored_data;
    size_t raw_size;
    size_t deflate_size;
    size_t rle_size;

    memset(&entry, 0, sizeof(entry));
    raw_data = NULL;
    deflate_data = NULL;
    rle_data = NULL;
    stored_data = NULL;
    raw_size = 0;
    deflate_size = 0;
    rle_size = 0;

    log_item(opts, index, total, "pack %s", input->name);
    if (read_whole_file(input->path, input->name, &raw_data, &raw_size, &entry.checksum, opts) != 0) {
        diag_error("%s: %s", input->path, strerror(errno));
        return -1;
    }

    stored_data = raw_data;
    entry.name = input->name;
    entry.size = (uint64_t)raw_size;
    entry.stored_size = (uint64_t)raw_size;
    entry.flags = 0;

    if (opts->compress && raw_size > 0) {
        if (deflate_compress(raw_data, raw_size, opts->compression_level, &deflate_data, &deflate_size) == 0 && deflate_size < entry.stored_size) {
            stored_data = deflate_data;
            entry.stored_size = (uint64_t)deflate_size;
            entry.flags = PAK_FLAG_DEFLATE;
        }
        if (rle_compress(raw_data, raw_size, &rle_data, &rle_size) == 0 && rle_size < entry.stored_size) {
            stored_data = rle_data;
            entry.stored_size = (uint64_t)rle_size;
            entry.flags = PAK_FLAG_RLE;
        }
        if (entry.flags != 0) {
            log_step(opts, "compressed %s with %s: %llu -> %llu bytes", input->name, entry_method(&entry), (unsigned long long)entry.size, (unsigned long long)entry.stored_size);
        }
    }

    if (write_entry_data(archive, &entry, stored_data) != 0) {
        diag_error("failed while packing '%s'", input->path);
        free(raw_data);
        free(deflate_data);
        free(rle_data);
        return -1;
    }

    free(raw_data);
    free(deflate_data);
    free(rle_data);
    return 0;
}

static int copy_entry_bytes(FILE *in, FILE *out, uint64_t size, uint32_t *crc)
{
    unsigned char buf[COPY_BUF_SIZE];
    uint64_t left = size;

    while (left > 0) {
        size_t want = left > COPY_BUF_SIZE ? COPY_BUF_SIZE : (size_t)left;
        size_t got = fread(buf, 1, want, in);

        if (got == 0) {
            return -1;
        }
        if (fwrite(buf, 1, got, out) != got) {
            return -1;
        }
        if (crc != NULL) {
            *crc = crc32_update(*crc, buf, got);
        }
        left -= got;
    }

    return 0;
}

static int copy_kept_entry(FILE *archive, FILE *out, const struct old_entry_ref *ref, int version)
{
    struct pak_entry entry;
    long header_pos;
    long end_pos;
    uint32_t crc;

    if (fseek(archive, ref->data_offset, SEEK_SET) != 0) {
        return -1;
    }

    if (version >= 2) {
        if (write_entry_header(out, &ref->entry) != 0) {
            return -1;
        }
        return copy_entry_bytes(archive, out, ref->entry.stored_size, NULL);
    }

    entry = ref->entry;
    entry.flags = 0;
    entry.stored_size = entry.size;
    entry.checksum = 0;

    header_pos = ftell(out);
    if (header_pos < 0 || write_entry_header(out, &entry) != 0) {
        return -1;
    }

    crc = crc32_start();
    if (copy_entry_bytes(archive, out, entry.stored_size, &crc) != 0) {
        return -1;
    }
    entry.checksum = crc32_finish(crc);

    end_pos = ftell(out);
    if (end_pos < 0 || fseek(out, header_pos, SEEK_SET) != 0 || write_entry_header(out, &entry) != 0 || fseek(out, end_pos, SEEK_SET) != 0) {
        return -1;
    }

    return 0;
}

static int entry_replaced_by_inputs(const char *name, const struct make_input *inputs, int input_count)
{
    int i;

    for (i = 0; i < input_count; i++) {
        if (same_archive_name(name, inputs[i].name)) {
            return 1;
        }
    }
    return 0;
}

static int read_archive_entries(FILE *archive, const char *archive_path, struct old_entry_ref **out_entries, uint32_t *out_count, int *out_version)
{
    struct old_entry_ref *entries;
    uint32_t count;
    uint32_t i;
    int version;

    *out_entries = NULL;
    *out_count = 0;

    if (read_archive_header(archive, &version, &count) != 0) {
        diag_error("bad archive '%s'", archive_path);
        return -1;
    }

    entries = calloc(count == 0 ? 1 : count, sizeof(*entries));
    if (entries == NULL) {
        diag_error("out of memory");
        return -1;
    }

    for (i = 0; i < count; i++) {
        struct pak_entry entry;
        long data_offset;

        if (read_entry_header(archive, version, &entry) != 0) {
            diag_error("damaged entry in '%s'", archive_path);
            free_old_entry_refs(entries, i);
            return -1;
        }

        data_offset = ftell(archive);
        if (data_offset < 0) {
            free_entry(&entry);
            free_old_entry_refs(entries, i);
            return -1;
        }

        entries[i].entry = entry;
        entries[i].data_offset = data_offset;
        if (skip_bytes(archive, entry.stored_size) != 0) {
            diag_error("damaged data in '%s'", archive_path);
            free_old_entry_refs(entries, i + 1);
            return -1;
        }
    }

    *out_entries = entries;
    *out_count = count;
    *out_version = version;
    return 0;
}

static int read_old_entries(FILE *archive, const char *archive_path, const struct make_input *inputs, int input_count, struct old_entry_ref **out_entries, uint32_t *out_old_count, uint32_t *out_kept_count, int *out_version)
{
    struct old_entry_ref *entries;
    uint32_t count;
    uint32_t kept_count;
    uint32_t i;

    if (read_archive_entries(archive, archive_path, &entries, &count, out_version) != 0) {
        return -1;
    }

    kept_count = 0;
    for (i = 0; i < count; i++) {
        if (entry_replaced_by_inputs(entries[i].entry.name, inputs, input_count)) {
            free_entry(&entries[i].entry);
            continue;
        }
        if (kept_count != i) {
            entries[kept_count] = entries[i];
        }
        kept_count++;
    }

    if (kept_count > 1) {
        qsort(entries, kept_count, sizeof(*entries), compare_old_entries_by_name);
    }

    *out_entries = entries;
    *out_old_count = count;
    *out_kept_count = kept_count;
    return 0;
}

static char *make_temp_archive_path(const char *archive_path)
{
    char *out;
    size_t len = strlen(archive_path);

    out = malloc(len + 5);
    if (out == NULL) {
        return NULL;
    }
    snprintf(out, len + 5, "%s.tmp", archive_path);
    return out;
}

static int replace_archive_file(const char *tmp_path, const char *archive_path)
{
#ifdef _WIN32
    if (MoveFileExA(tmp_path, archive_path, MOVEFILE_REPLACE_EXISTING) != 0) {
        return 0;
    }
    errno = EACCES;
    return -1;
#else
    return rename(tmp_path, archive_path);
#endif
}

static int rewrite_archive_file(const char *archive_path, FILE **archive, struct old_entry_ref *entries, uint32_t entry_count, const unsigned char *keep, uint32_t kept_count, int version, const struct pak_options *opts, const char *item_verb)
{
    FILE *out;
    char *tmp_path;
    uint32_t i;
    uint32_t written;
    int rc;

    tmp_path = make_temp_archive_path(archive_path);
    if (tmp_path == NULL) {
        diag_error("out of memory");
        return -1;
    }

    out = fopen(tmp_path, "wb");
    if (out == NULL) {
        diag_error("%s: %s", tmp_path, strerror(errno));
        free(tmp_path);
        return -1;
    }

    rc = -1;
    if (write_archive_header(out, kept_count) != 0) {
        diag_error("failed to write archive header");
        goto done;
    }

    written = 0;
    for (i = 0; i < entry_count; i++) {
        if (keep != NULL && !keep[i]) {
            continue;
        }
        written++;
        if (item_verb != NULL) {
            log_item(opts, (int)written, (int)kept_count, "%s %s", item_verb, entries[i].entry.name);
        } else {
            log_count_progress(opts, "rewrite", written, kept_count, written == kept_count);
        }
        if (copy_kept_entry(*archive, out, &entries[i], version) != 0) {
            diag_error("failed while writing '%s'", entries[i].entry.name);
            goto done;
        }
    }
    if (kept_count == 0 && item_verb == NULL) {
        log_count_progress(opts, "rewrite", 0, 0, 1);
    }

    if (fclose(out) != 0) {
        out = NULL;
        diag_error("failed to finish %s", tmp_path);
        goto done;
    }
    out = NULL;

    if (*archive != NULL && fclose(*archive) != 0) {
        *archive = NULL;
        diag_error("failed to close %s", archive_path);
        goto done;
    }
    *archive = NULL;

    if (replace_archive_file(tmp_path, archive_path) != 0) {
        diag_error("failed to replace %s: %s", archive_path, strerror(errno));
        goto done;
    }

    rc = 0;

done:
    if (out != NULL) {
        fclose(out);
    }
    if (*archive != NULL) {
        fclose(*archive);
        *archive = NULL;
    }
    if (rc != 0) {
        remove(tmp_path);
    }
    free(tmp_path);
    return rc;
}

int pak_make(const char *archive_path, int file_count, char **file_paths, const struct pak_options *opts)
{
    FILE *archive;
    struct make_input *inputs;
    int input_count;
    int i;

    if (file_count <= 0) {
        diag_error("no input files");
        return -1;
    }

    if (build_make_inputs(archive_path, file_count, file_paths, opts, &inputs, &input_count) != 0) {
        return -1;
    }
    if (input_count <= 0) {
        diag_error("no files left after ignores");
        free_make_inputs(inputs, input_count);
        return -1;
    }

    log_step(opts, "create %s", archive_path);
    archive = fopen(archive_path, "wb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        free_make_inputs(inputs, input_count);
        return -1;
    }

    if (write_archive_header(archive, (uint32_t)input_count) != 0) {
        diag_error("failed to write archive header");
        fclose(archive);
        free_make_inputs(inputs, input_count);
        return -1;
    }

    log_step(opts, "pack %d file%s", input_count, input_count == 1 ? "" : "s");
    for (i = 0; i < input_count; i++) {
        if (pack_file_entry(archive, &inputs[i], i + 1, input_count, opts) != 0) {
            fclose(archive);
            free_make_inputs(inputs, input_count);
            return -1;
        }
    }

    if (fclose(archive) != 0) {
        diag_error("failed to finish %s", archive_path);
        free_make_inputs(inputs, input_count);
        return -1;
    }

    log_step(opts, "done");
    free_make_inputs(inputs, input_count);
    return 0;
}

int pak_update(const char *archive_path, int file_count, char **file_paths, const struct pak_options *opts)
{
    FILE *archive;
    FILE *out;
    struct make_input *inputs;
    struct old_entry_ref *old_entries;
    char *tmp_path;
    uint64_t final_count64;
    uint32_t old_count;
    uint32_t kept_count;
    uint32_t final_count;
    uint32_t old_index;
    int input_count;
    int input_index;
    int version;
    int written;
    int rc;

    if (!io_file_exists(archive_path)) {
        log_step(opts, "archive missing, create %s", archive_path);
        return pak_make(archive_path, file_count, file_paths, opts);
    }

    if (build_make_inputs(archive_path, file_count, file_paths, opts, &inputs, &input_count) != 0) {
        return -1;
    }
    if (input_count <= 0) {
        diag_error("no files left after ignores");
        free_make_inputs(inputs, input_count);
        return -1;
    }

    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        free_make_inputs(inputs, input_count);
        return -1;
    }

    old_entries = NULL;
    if (read_old_entries(archive, archive_path, inputs, input_count, &old_entries, &old_count, &kept_count, &version) != 0) {
        fclose(archive);
        free_make_inputs(inputs, input_count);
        return -1;
    }

    final_count64 = (uint64_t)kept_count + (uint64_t)input_count;
    if (final_count64 > UINT32_MAX) {
        diag_error("too many files");
        fclose(archive);
        free_old_entry_refs(old_entries, kept_count);
        free_make_inputs(inputs, input_count);
        return -1;
    }
    final_count = (uint32_t)final_count64;

    tmp_path = make_temp_archive_path(archive_path);
    if (tmp_path == NULL) {
        diag_error("out of memory");
        fclose(archive);
        free_old_entry_refs(old_entries, kept_count);
        free_make_inputs(inputs, input_count);
        return -1;
    }

    out = fopen(tmp_path, "wb");
    if (out == NULL) {
        diag_error("%s: %s", tmp_path, strerror(errno));
        free(tmp_path);
        fclose(archive);
        free_old_entry_refs(old_entries, kept_count);
        free_make_inputs(inputs, input_count);
        return -1;
    }

    rc = -1;
    log_step(opts, "update %s", archive_path);
    log_step(opts, "keep %u, replace %u, write %d", (unsigned)kept_count, (unsigned)(old_count - kept_count), input_count);

    if (write_archive_header(out, final_count) != 0) {
        diag_error("failed to write archive header");
        goto done;
    }

    old_index = 0;
    input_index = 0;
    written = 0;
    while (old_index < kept_count || input_index < input_count) {
        int use_old = 0;

        if (old_index < kept_count && input_index >= input_count) {
            use_old = 1;
        } else if (old_index < kept_count && strcmp(old_entries[old_index].entry.name, inputs[input_index].name) <= 0) {
            use_old = 1;
        }

        written++;
        if (use_old) {
            log_item(opts, written, (int)final_count, "keep %s", old_entries[old_index].entry.name);
            if (copy_kept_entry(archive, out, &old_entries[old_index], version) != 0) {
                diag_error("failed while keeping '%s'", old_entries[old_index].entry.name);
                goto done;
            }
            old_index++;
        } else {
            if (pack_file_entry(out, &inputs[input_index], written, (int)final_count, opts) != 0) {
                goto done;
            }
            input_index++;
        }
    }

    if (fclose(out) != 0) {
        out = NULL;
        diag_error("failed to finish %s", tmp_path);
        goto done;
    }
    out = NULL;
    fclose(archive);
    archive = NULL;

    if (replace_archive_file(tmp_path, archive_path) != 0) {
        diag_error("failed to replace %s: %s", archive_path, strerror(errno));
        goto done;
    }

    log_step(opts, "done");
    rc = 0;

done:
    if (out != NULL) {
        fclose(out);
    }
    if (archive != NULL) {
        fclose(archive);
    }
    if (rc != 0) {
        remove(tmp_path);
    }
    free(tmp_path);
    free_old_entry_refs(old_entries, kept_count);
    free_make_inputs(inputs, input_count);
    return rc;
}

static int max_int(int left, int right)
{
    return left > right ? left : right;
}

static void format_list_size(char *buf, size_t buf_size, uint64_t bytes)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double value = (double)bytes;
    int unit = 0;

    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        unit++;
    }

    if (unit == 0) {
        snprintf(buf, buf_size, "%llu B", (unsigned long long)bytes);
    } else {
        snprintf(buf, buf_size, "%.1f %s", value, units[unit]);
    }
}

static void format_list_ratio(char *buf, size_t buf_size, uint64_t stored_size, uint64_t size)
{
    if (size == 0) {
        snprintf(buf, buf_size, "0.0%%");
        return;
    }
    snprintf(buf, buf_size, "%.1f%%", (double)stored_size * 100.0 / (double)size);
}

static int entry_is_selected(const char *entry_name, int selected_count, char **selected_names);

static int list_name_width(int widest_name)
{
    if (widest_name < 24) {
        return 24;
    }
    if (widest_name > 40) {
        return 40;
    }
    return widest_name;
}

static void print_chars(int ch, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        putchar(ch);
    }
}

static void print_long_list_header(int name_width)
{
    printf("%s%-*s  %10s  %10s  %7s  %-7s  %8s%s\n", out_clr(PAK_CLR_BOLD PAK_CLR_CYAN), name_width, "name", "size", "stored", "ratio", "method", "crc32", out_clr(PAK_CLR_RESET));
    print_chars('-', name_width);
    printf("  ----------  ----------  -------  -------  --------\n");
}

static void format_list_name(char *buf, size_t buf_size, const char *name, int width)
{
    size_t len;
    size_t left;
    size_t right;

    len = strlen(name);
    if (buf_size == 0) {
        return;
    }
    if (len <= (size_t)width) {
        snprintf(buf, buf_size, "%s", name);
        return;
    }

    if (width <= 3) {
        snprintf(buf, buf_size, "%.*s", width, "...");
        return;
    }

    left = (size_t)(width - 3) / 2;
    right = (size_t)(width - 3) - left;
    if ((size_t)width + 1 > buf_size) {
        width = (int)buf_size - 1;
        left = (size_t)(width - 3) / 2;
        right = (size_t)(width - 3) - left;
    }

    memcpy(buf, name, left);
    memcpy(buf + left, "...", 3);
    memcpy(buf + left + 3, name + len - right, right);
    buf[left + 3 + right] = '\0';
}

static void format_list_crc(char *buf, size_t buf_size, const struct pak_entry *entry, int version)
{
    if (version == 1) {
        snprintf(buf, buf_size, "-");
    } else {
        snprintf(buf, buf_size, "%08x", entry->checksum);
    }
}

static void print_long_list_table_entry(const struct pak_entry *entry, int version, int name_width)
{
    char name[64];
    char size[16];
    char stored[16];
    char ratio[16];
    char crc[16];

    format_list_name(name, sizeof(name), entry->name, name_width);
    format_list_size(size, sizeof(size), entry->size);
    format_list_size(stored, sizeof(stored), entry->stored_size);
    format_list_ratio(ratio, sizeof(ratio), entry->stored_size, entry->size);
    format_list_crc(crc, sizeof(crc), entry, version);

    printf("%s%-*s%s  %s%10s%s  %s%10s%s  %s%7s%s  %s%-7s%s  %s%8s%s\n", out_clr(PAK_CLR_BOLD), name_width, name, out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_GREEN), size, out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_GREEN), stored, out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_YELLOW), ratio, out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_BOLD), entry_method(entry), out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_DIM), crc, out_clr(PAK_CLR_RESET));
}

static void print_long_list_block_header(void)
{
    printf("%sentries%s\n", out_clr(PAK_CLR_BOLD PAK_CLR_CYAN), out_clr(PAK_CLR_RESET));
    printf("-------\n");
}

static void print_long_list_block_entry(const struct pak_entry *entry, int version)
{
    char size[16];
    char stored[16];
    char ratio[16];
    char crc[16];

    format_list_size(size, sizeof(size), entry->size);
    format_list_size(stored, sizeof(stored), entry->stored_size);
    format_list_ratio(ratio, sizeof(ratio), entry->stored_size, entry->size);
    format_list_crc(crc, sizeof(crc), entry, version);

    printf("%s%s%s\n", out_clr(PAK_CLR_BOLD), entry->name, out_clr(PAK_CLR_RESET));
    printf("  %s%-7s%s %s%10s%s    %s%-7s%s %s%10s%s    %s%-7s%s %s%8s%s\n", out_clr(PAK_CLR_CYAN), "size", out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_GREEN), size, out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_CYAN), "stored", out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_GREEN), stored, out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_CYAN), "ratio", out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_YELLOW), ratio, out_clr(PAK_CLR_RESET));
    printf("  %s%-7s%s %s%10s%s    %s%-7s%s %s%10s%s\n", out_clr(PAK_CLR_CYAN), "method", out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_BOLD), entry_method(entry), out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_CYAN), "crc32", out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_DIM), crc, out_clr(PAK_CLR_RESET));
}

static int selected_name_matches(const char *entry_name, const char *selected_name)
{
    if (pak_pattern_has_magic(selected_name)) {
        return pak_pattern_match(selected_name, entry_name);
    }
    return strcmp(entry_name, selected_name) == 0;
}

static int entry_is_selected(const char *entry_name, int selected_count, char **selected_names)
{
    int i;

    if (selected_count == 0) {
        return 1;
    }

    for (i = 0; i < selected_count; i++) {
        if (selected_name_matches(entry_name, selected_names[i])) {
            return 1;
        }
    }

    return 0;
}

static void mark_selected(const char *entry_name, int selected_count, char **selected_names, int *seen)
{
    int i;

    for (i = 0; i < selected_count; i++) {
        if (selected_name_matches(entry_name, selected_names[i])) {
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
            if (pak_pattern_has_magic(selected_names[i])) {
                diag_error("no match for '%s'", selected_names[i]);
            } else {
                diag_error("not found '%s'", selected_names[i]);
            }
            missing = 1;
        }
    }

    return missing ? -1 : 0;
}

static const char *closest_list_entry_name(struct pak_entry *entries, uint32_t count, const char *query)
{
    const char *query_base = entry_base_name(query);
    const char *best = NULL;
    int best_score = 999;
    uint32_t i;

    for (i = 0; i < count; i++) {
        const char *entry_base = entry_base_name(entries[i].name);
        int score;

        if (strcmp(query_base, entry_base) == 0) {
            return entries[i].name;
        }

        score = archive_edit_distance(query, entries[i].name);
        if (score < best_score) {
            best_score = score;
            best = entries[i].name;
        }
    }

    if (best_score <= 3 || best_score <= (int)strlen(query) / 3) {
        return best;
    }
    return NULL;
}

static int report_missing_list_selected(const char *archive_path, int selected_count, char **selected_names, int *seen, struct pak_entry *entries, uint32_t count)
{
    int missing = 0;
    int i;

    for (i = 0; i < selected_count; i++) {
        const char *suggestion;

        if (seen[i]) {
            continue;
        }

        missing = 1;
        if (pak_pattern_has_magic(selected_names[i])) {
            diag_error("no match for '%s'", selected_names[i]);
            print_list_hint(archive_path, selected_names[i]);
            continue;
        }

        diag_error("not found '%s'", selected_names[i]);
        suggestion = closest_list_entry_name(entries, count, selected_names[i]);
        if (suggestion != NULL) {
            diag_hint("closest entry is '%s'", suggestion);
            print_command_entry_hint("list", archive_path, suggestion);
        } else {
            diag_hint("check the current archive names");
            print_list_search_hint(archive_path, selected_names[i]);
        }
    }

    return missing ? -1 : 0;
}

int pak_list(const char *archive_path, int selected_count, char **selected_names, const struct pak_options *opts)
{
    FILE *archive;
    struct pak_entry *entries;
    int *seen;
    uint32_t count;
    uint32_t i;
    int version;
    int name_width;
    int selected;

    seen = calloc(selected_count == 0 ? 1 : (size_t)selected_count, sizeof(*seen));
    if (seen == NULL) {
        diag_error("out of memory");
        return -1;
    }

    log_step(opts, "read %s", archive_path);
    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        free(seen);
        return -1;
    }

    if (read_archive_header(archive, &version, &count) != 0) {
        diag_error("bad archive '%s'", archive_path);
        fclose(archive);
        free(seen);
        return -1;
    }

    entries = calloc(count == 0 ? 1 : count, sizeof(*entries));
    if (entries == NULL) {
        diag_error("out of memory");
        fclose(archive);
        free(seen);
        return -1;
    }

    name_width = 4;
    for (i = 0; i < count; i++) {
        if (read_entry_header(archive, version, &entries[i]) != 0) {
            diag_error("damaged entry in '%s'", archive_path);
            goto fail;
        }

        selected = entry_is_selected(entries[i].name, selected_count, selected_names);
        if (selected) {
            mark_selected(entries[i].name, selected_count, selected_names, seen);
            name_width = max_int(name_width, (int)strlen(entries[i].name));
        }

        if (skip_bytes(archive, entries[i].stored_size) != 0) {
            diag_error("damaged data for '%s'", entries[i].name);
            goto fail;
        }
    }

    if (report_missing_list_selected(archive_path, selected_count, selected_names, seen, entries, count) != 0) {
        goto fail;
    }

    if (opts->long_list) {
        name_width = list_name_width(name_width);
        if (opts->full_names) {
            int printed_block = 0;

            print_long_list_block_header();
            for (i = 0; i < count; i++) {
                if (entry_is_selected(entries[i].name, selected_count, selected_names)) {
                    if (printed_block) {
                        putchar('\n');
                    }
                    print_long_list_block_entry(&entries[i], version);
                    printed_block = 1;
                }
            }
        } else {
            print_long_list_header(name_width);
            for (i = 0; i < count; i++) {
                if (entry_is_selected(entries[i].name, selected_count, selected_names)) {
                    print_long_list_table_entry(&entries[i], version, name_width);
                }
            }
        }
    } else {
        for (i = 0; i < count; i++) {
            if (entry_is_selected(entries[i].name, selected_count, selected_names)) {
                printf("%s%-*s%s  %s%llu bytes%s\n", out_clr(PAK_CLR_BOLD), name_width, entries[i].name, out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_GREEN), (unsigned long long)entries[i].size, out_clr(PAK_CLR_RESET));
            }
        }
    }

    for (i = 0; i < count; i++) {
        free_entry(&entries[i]);
    }
    free(entries);
    free(seen);
    fclose(archive);
    return 0;

fail:
    for (i = 0; i < count; i++) {
        free_entry(&entries[i]);
    }
    free(entries);
    free(seen);
    fclose(archive);
    return -1;
}
static int process_entry_data(FILE *archive, FILE *out, const struct pak_entry *entry, int version, const struct pak_options *opts, int check_crc)
{
    uint32_t crc = crc32_start();
    uint32_t actual;

    if ((entry->flags & PAK_FLAG_DEFLATE) != 0) {
        if (deflate_decompress(archive, out, entry->stored_size, entry->size, &crc, entry->name, opts) != 0) {
            return -1;
        }
    } else if ((entry->flags & PAK_FLAG_RLE) != 0) {
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
        diag_error("checksum mismatch for '%s'", entry->name);
        return -1;
    }

    return 0;
}

static uint32_t checksum_bytes(const unsigned char *data, size_t size)
{
    uint32_t crc = crc32_start();

    crc = crc32_update(crc, data, size);
    return crc32_finish(crc);
}

static int read_entry_data_to_memory(FILE *archive, const struct old_entry_ref *ref, int version, unsigned char **out_data, size_t *out_size, const struct pak_options *opts)
{
    struct pak_options quiet_opts;
    unsigned char *data;
    size_t size;
    FILE *tmp;

    if (ref->entry.size > SIZE_MAX) {
        diag_error("'%s' is too large to repack", ref->entry.name);
        return -1;
    }

    tmp = tmpfile();
    if (tmp == NULL) {
        diag_error("could not prepare '%s' for repacking", ref->entry.name);
        return -1;
    }

    quiet_opts = *opts;
    quiet_opts.quiet = 1;
    if (fseek(archive, ref->data_offset, SEEK_SET) != 0 || process_entry_data(archive, tmp, &ref->entry, version, &quiet_opts, version >= 2) != 0 || fflush(tmp) != 0) {
        fclose(tmp);
        diag_error("failed while reading '%s'", ref->entry.name);
        return -1;
    }

    size = (size_t)ref->entry.size;
    data = malloc(size == 0 ? 1 : size);
    if (data == NULL) {
        fclose(tmp);
        diag_error("out of memory");
        return -1;
    }

    rewind(tmp);
    if (size > 0 && fread(data, 1, size, tmp) != size) {
        free(data);
        fclose(tmp);
        diag_error("failed while reading '%s'", ref->entry.name);
        return -1;
    }
    if (ferror(tmp)) {
        free(data);
        fclose(tmp);
        diag_error("failed while reading '%s'", ref->entry.name);
        return -1;
    }

    fclose(tmp);
    *out_data = data;
    *out_size = size;
    return 0;
}

static int write_repacked_entry(FILE *archive, FILE *out, const struct old_entry_ref *ref, int version, int index, int total, const struct pak_options *opts)
{
    struct pak_entry entry;
    unsigned char *raw_data;
    unsigned char *deflate_data;
    unsigned char *rle_data;
    unsigned char *stored_data;
    size_t raw_size;
    size_t deflate_size;
    size_t rle_size;
    int rc;

    raw_data = NULL;
    deflate_data = NULL;
    rle_data = NULL;
    stored_data = NULL;
    raw_size = 0;
    deflate_size = 0;
    rle_size = 0;

    log_item(opts, index, total, "repack %s", ref->entry.name);
    if (read_entry_data_to_memory(archive, ref, version, &raw_data, &raw_size, opts) != 0) {
        return -1;
    }

    entry = ref->entry;
    entry.size = (uint64_t)raw_size;
    entry.stored_size = (uint64_t)raw_size;
    entry.flags = 0;
    entry.checksum = checksum_bytes(raw_data, raw_size);
    stored_data = raw_data;

    if (!opts->store && opts->compress && raw_size > 0) {
        if (deflate_compress(raw_data, raw_size, opts->compression_level, &deflate_data, &deflate_size) == 0 && deflate_size < entry.stored_size) {
            stored_data = deflate_data;
            entry.stored_size = (uint64_t)deflate_size;
            entry.flags = PAK_FLAG_DEFLATE;
        }
        if (rle_compress(raw_data, raw_size, &rle_data, &rle_size) == 0 && rle_size < entry.stored_size) {
            stored_data = rle_data;
            entry.stored_size = (uint64_t)rle_size;
            entry.flags = PAK_FLAG_RLE;
        }
        if (entry.flags != 0) {
            log_step(opts, "compressed %s with %s: %llu -> %llu bytes", entry.name, entry_method(&entry), (unsigned long long)entry.size, (unsigned long long)entry.stored_size);
        }
    }

    rc = 0;
    if (write_entry_data(out, &entry, stored_data) != 0) {
        diag_error("failed while writing '%s'", ref->entry.name);
        rc = -1;
    }

    free(raw_data);
    free(deflate_data);
    free(rle_data);
    return rc;
}

static int rewrite_repacked_archive_file(const char *archive_path, FILE **archive, struct old_entry_ref *entries, uint32_t count, const unsigned char *repack, uint32_t repack_count, int version, const struct pak_options *opts)
{
    FILE *out;
    char *tmp_path;
    uint32_t i;
    uint32_t repacked;
    int rc;

    tmp_path = make_temp_archive_path(archive_path);
    if (tmp_path == NULL) {
        diag_error("out of memory");
        return -1;
    }

    out = fopen(tmp_path, "wb");
    if (out == NULL) {
        diag_error("%s: %s", tmp_path, strerror(errno));
        free(tmp_path);
        return -1;
    }

    rc = -1;
    if (write_archive_header(out, count) != 0) {
        diag_error("failed to write archive header");
        goto done;
    }

    repacked = 0;
    for (i = 0; i < count; i++) {
        if (repack[i]) {
            repacked++;
            if (write_repacked_entry(*archive, out, &entries[i], version, (int)repacked, (int)repack_count, opts) != 0) {
                goto done;
            }
        } else if (copy_kept_entry(*archive, out, &entries[i], version) != 0) {
            diag_error("failed while writing '%s'", entries[i].entry.name);
            goto done;
        }
    }

    if (fclose(out) != 0) {
        out = NULL;
        diag_error("failed to finish %s", tmp_path);
        goto done;
    }
    out = NULL;

    if (*archive != NULL && fclose(*archive) != 0) {
        *archive = NULL;
        diag_error("failed to close %s", archive_path);
        goto done;
    }
    *archive = NULL;

    if (replace_archive_file(tmp_path, archive_path) != 0) {
        diag_error("failed to replace %s: %s", archive_path, strerror(errno));
        goto done;
    }

    rc = 0;

done:
    if (out != NULL) {
        fclose(out);
    }
    if (*archive != NULL) {
        fclose(*archive);
        *archive = NULL;
    }
    if (rc != 0) {
        remove(tmp_path);
    }
    free(tmp_path);
    return rc;
}

int pak_repack(const char *archive_path, int selected_count, char **selected_names, const struct pak_options *opts)
{
    FILE *archive;
    struct old_entry_ref *entries;
    struct pak_options repack_opts;
    unsigned char *repack;
    int *seen;
    uint32_t count;
    uint32_t repack_count;
    uint32_t i;
    int version;
    int rc;

    if (opts->store && opts->compress) {
        diag_error("repack: choose --store or --compress");
        diag_hint("--store writes plain entries; --compress chooses compressed storage when smaller");
        return -1;
    }

    repack_opts = *opts;
    if (!repack_opts.store) {
        repack_opts.compress = 1;
    }

    seen = calloc(selected_count == 0 ? 1 : (size_t)selected_count, sizeof(*seen));
    if (seen == NULL) {
        diag_error("out of memory");
        return -1;
    }

    log_step(&repack_opts, "read %s", archive_path);
    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        free(seen);
        return -1;
    }

    entries = NULL;
    if (read_archive_entries(archive, archive_path, &entries, &count, &version) != 0) {
        fclose(archive);
        free(seen);
        return -1;
    }

    repack = calloc(count == 0 ? 1 : count, sizeof(*repack));
    if (repack == NULL) {
        diag_error("out of memory");
        free_old_entry_refs(entries, count);
        fclose(archive);
        free(seen);
        return -1;
    }

    repack_count = 0;
    for (i = 0; i < count; i++) {
        if (entry_is_selected(entries[i].entry.name, selected_count, selected_names)) {
            mark_selected(entries[i].entry.name, selected_count, selected_names, seen);
            repack[i] = 1;
            repack_count++;
        }
    }

    rc = -1;
    for (i = 0; i < (uint32_t)selected_count; i++) {
        if (!seen[i]) {
            report_missing_entry_with_hints("repack", archive_path, selected_names[i], entries, count);
            goto done;
        }
    }

    if (repack_count == 0) {
        log_step(&repack_opts, "nothing to repack");
        rc = 0;
        goto done;
    }

    if (repack_opts.store) {
        log_step(&repack_opts, "store %u of %u file%s in %s", (unsigned)repack_count, (unsigned)count, repack_count == 1 ? "" : "s", archive_path);
    } else {
        log_step(&repack_opts, "compress %u of %u file%s in %s", (unsigned)repack_count, (unsigned)count, repack_count == 1 ? "" : "s", archive_path);
    }
    rc = rewrite_repacked_archive_file(archive_path, &archive, entries, count, repack, repack_count, version, &repack_opts);
    if (rc == 0) {
        log_step(&repack_opts, "done");
    }

done:
    if (archive != NULL) {
        fclose(archive);
    }
    free(repack);
    free(seen);
    free_old_entry_refs(entries, count);
    return rc;
}

static int is_text_byte(unsigned char ch)
{
    if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '\f' || ch == '\b') {
        return 1;
    }
    if (ch >= 32) {
        return 1;
    }
    return 0;
}

static int file_looks_binary(FILE *fp, int *binary)
{
    unsigned char buf[4096];
    uint64_t checked;
    uint64_t odd;

    checked = 0;
    odd = 0;
    *binary = 0;

    rewind(fp);
    for (;;) {
        size_t got = fread(buf, 1, sizeof(buf), fp);
        size_t i;

        if (got == 0) {
            if (ferror(fp)) {
                return -1;
            }
            break;
        }

        for (i = 0; i < got; i++) {
            if (buf[i] == 0) {
                *binary = 1;
                rewind(fp);
                return 0;
            }
            if (!is_text_byte(buf[i])) {
                odd++;
            }
        }
        checked += got;
        if (checked >= 4096 && odd * 100 > checked * 5) {
            *binary = 1;
            rewind(fp);
            return 0;
        }
    }

    if (checked > 0 && odd * 100 > checked * 5) {
        *binary = 1;
    }
    rewind(fp);
    return 0;
}

static int copy_temp_to_stdout(FILE *fp)
{
    unsigned char buf[COPY_BUF_SIZE];

    rewind(fp);
    for (;;) {
        size_t got = fread(buf, 1, sizeof(buf), fp);

        if (got == 0) {
            return ferror(fp) ? -1 : 0;
        }
        if (fwrite(buf, 1, got, stdout) != got) {
            return -1;
        }
    }
}

static int cat_entry_to_terminal(FILE *archive, const char *archive_path, const struct old_entry_ref *ref, int version, const struct pak_options *opts)
{
    FILE *tmp;
    int binary;

    tmp = tmpfile();
    if (tmp == NULL) {
        diag_error("could not inspect '%s' before printing", ref->entry.name);
        return -1;
    }

    if (process_entry_data(archive, tmp, &ref->entry, version, opts, 1) != 0 || fflush(tmp) != 0) {
        fclose(tmp);
        diag_error("failed while reading '%s'", ref->entry.name);
        return -1;
    }

    if (file_looks_binary(tmp, &binary) != 0) {
        fclose(tmp);
        diag_error("failed while checking '%s'", ref->entry.name);
        return -1;
    }
    if (binary) {
        fclose(tmp);
        diag_error("'%s' looks like binary data; cat would dump it into your terminal", ref->entry.name);
        print_cat_redirect_hint(archive_path, ref->entry.name);
        print_unpack_hint(archive_path, ref->entry.name);
        return -1;
    }

    if (copy_temp_to_stdout(tmp) != 0) {
        fclose(tmp);
        diag_error("failed while printing '%s'", ref->entry.name);
        return -1;
    }

    fclose(tmp);
    return 0;
}

int pak_cat(const char *archive_path, const char *entry_name, const struct pak_options *opts)
{
    FILE *archive;
    struct old_entry_ref *entries;
    struct pak_options quiet_opts;
    uint32_t count;
    uint32_t i;
    uint32_t match_index;
    uint32_t match_count;
    int version;
    int rc;

    quiet_opts = *opts;
    quiet_opts.quiet = 1;

#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        return -1;
    }

    entries = NULL;
    if (read_archive_entries(archive, archive_path, &entries, &count, &version) != 0) {
        fclose(archive);
        return -1;
    }

    match_index = 0;
    match_count = 0;
    for (i = 0; i < count; i++) {
        if (selected_name_matches(entries[i].entry.name, entry_name)) {
            match_index = i;
            match_count++;
        }
    }

    if (match_count == 0) {
        report_missing_entry_with_hints("cat", archive_path, entry_name, entries, count);
        free_old_entry_refs(entries, count);
        fclose(archive);
        return -1;
    }

    if (match_count > 1) {
        diag_error("cat: '%s' matched %u files", entry_name, (unsigned)match_count);
        diag_hint("cat prints one file; use list or unpack for patterns");
        print_list_hint(archive_path, entry_name);
        print_unpack_hint(archive_path, entry_name);
        free_old_entry_refs(entries, count);
        fclose(archive);
        return -1;
    }

    rc = -1;
    if (stdout_is_tty() && entries[match_index].entry.size > 1024ull * 1024ull) {
        diag_error("'%s' is %llu bytes; cat would dump it into your terminal", entries[match_index].entry.name, (unsigned long long)entries[match_index].entry.size);
        print_cat_redirect_hint(archive_path, entries[match_index].entry.name);
        print_unpack_hint(archive_path, entries[match_index].entry.name);
        goto done;
    }
    if (fseek(archive, entries[match_index].data_offset, SEEK_SET) != 0) {
        diag_error("damaged data for '%s'", entries[match_index].entry.name);
        goto done;
    }
    if (stdout_is_tty()) {
        if (cat_entry_to_terminal(archive, archive_path, &entries[match_index], version, &quiet_opts) != 0) {
            goto done;
        }
    } else if (process_entry_data(archive, stdout, &entries[match_index].entry, version, &quiet_opts, 1) != 0) {
        diag_error("failed while reading '%s'", entries[match_index].entry.name);
        goto done;
    }
    fflush(stdout);
    rc = 0;

done:
    free_old_entry_refs(entries, count);
    fclose(archive);
    return rc;
}

int pak_delete(const char *archive_path, int selected_count, char **selected_names, const struct pak_options *opts)
{
    FILE *archive;
    struct old_entry_ref *entries;
    unsigned char *keep;
    int *seen;
    uint32_t count;
    uint32_t kept_count;
    uint32_t matched_count;
    uint32_t i;
    int version;
    int rc;

    if (selected_count <= 0) {
        diag_error("delete: missing file name or pattern");
        return -1;
    }

    seen = calloc((size_t)selected_count, sizeof(*seen));
    if (seen == NULL) {
        diag_error("out of memory");
        return -1;
    }

    log_step(opts, "read %s", archive_path);
    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        free(seen);
        return -1;
    }

    entries = NULL;
    if (read_archive_entries(archive, archive_path, &entries, &count, &version) != 0) {
        fclose(archive);
        free(seen);
        return -1;
    }

    keep = calloc(count == 0 ? 1 : count, sizeof(*keep));
    if (keep == NULL) {
        diag_error("out of memory");
        free_old_entry_refs(entries, count);
        fclose(archive);
        free(seen);
        return -1;
    }

    kept_count = 0;
    matched_count = 0;
    for (i = 0; i < count; i++) {
        if (entry_is_selected(entries[i].entry.name, selected_count, selected_names)) {
            mark_selected(entries[i].entry.name, selected_count, selected_names, seen);
            matched_count++;
        } else {
            keep[i] = 1;
            kept_count++;
        }
    }

    rc = -1;
    for (i = 0; i < (uint32_t)selected_count; i++) {
        if (!seen[i]) {
            report_missing_entry_with_hints("delete", archive_path, selected_names[i], entries, count);
            goto done;
        }
    }

    if (matched_count == 0) {
        goto done;
    }

    log_step(opts, "delete %u of %u file%s from %s", (unsigned)matched_count, (unsigned)count, count == 1 ? "" : "s", archive_path);
    log_step(opts, "rewrite %u file%s", (unsigned)kept_count, kept_count == 1 ? "" : "s");
    rc = rewrite_archive_file(archive_path, &archive, entries, count, keep, kept_count, version, opts, NULL);
    if (rc == 0) {
        log_step(opts, "done");
    }

done:
    if (archive != NULL) {
        fclose(archive);
    }
    free(keep);
    free(seen);
    free_old_entry_refs(entries, count);
    return rc;
}

int pak_rename(const char *archive_path, const char *old_name, const char *new_name, const struct pak_options *opts)
{
    FILE *archive;
    struct old_entry_ref *entries;
    char *name_copy;
    uint32_t count;
    uint32_t i;
    uint32_t match_index;
    uint32_t match_count;
    int version;
    int rc;

    if (!io_is_safe_path(new_name)) {
        diag_error("bad archive path '%s'", new_name);
        return -1;
    }

    log_step(opts, "read %s", archive_path);
    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        return -1;
    }

    entries = NULL;
    if (read_archive_entries(archive, archive_path, &entries, &count, &version) != 0) {
        fclose(archive);
        return -1;
    }

    match_index = 0;
    match_count = 0;
    for (i = 0; i < count; i++) {
        if (selected_name_matches(entries[i].entry.name, old_name)) {
            match_index = i;
            match_count++;
        }
    }

    rc = -1;
    if (match_count == 0) {
        report_missing_rename_source(archive_path, old_name, new_name, entries, count);
        goto done;
    }
    if (match_count > 1) {
        diag_error("rename: '%s' matched %u files", old_name, (unsigned)match_count);
        diag_hint("rename needs one source entry");
        goto done;
    }
    if (same_archive_name(entries[match_index].entry.name, new_name)) {
        diag_error("rename: source and target are the same");
        goto done;
    }

    for (i = 0; i < count; i++) {
        if (i != match_index && same_archive_name(entries[i].entry.name, new_name)) {
            diag_error("entry '%s' already exists", new_name);
            goto done;
        }
    }

    name_copy = archive_string_copy(new_name);
    if (name_copy == NULL) {
        diag_error("out of memory");
        goto done;
    }

    log_step(opts, "rename %s -> %s", entries[match_index].entry.name, new_name);
    free(entries[match_index].entry.name);
    entries[match_index].entry.name = name_copy;

    log_step(opts, "rewrite %u file%s", (unsigned)count, count == 1 ? "" : "s");
    rc = rewrite_archive_file(archive_path, &archive, entries, count, NULL, count, version, opts, NULL);
    if (rc == 0) {
        log_step(opts, "done");
    }

done:
    if (archive != NULL) {
        fclose(archive);
    }
    free_old_entry_refs(entries, count);
    return rc;
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
        diag_error("out of memory");
        return -1;
    }

    log_step(opts, "read %s", archive_path);
    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        free(seen);
        return -1;
    }

    if (read_archive_header(archive, &version, &count) != 0) {
        diag_error("bad archive '%s'", archive_path);
        fclose(archive);
        free(seen);
        return -1;
    }

    if (selected_count == 0) {
        log_step(opts, "extract %u file%s", count, count == 1 ? "" : "s");
    } else {
        log_step(opts, "extract matches for %d selection%s", selected_count, selected_count == 1 ? "" : "s");
    }
    for (i = 0; i < count; i++) {
        struct pak_entry entry;
        char *out_path;
        FILE *out;

        if (read_entry_header(archive, version, &entry) != 0) {
            diag_error("damaged entry in '%s'", archive_path);
            fclose(archive);
            free(seen);
            return -1;
        }

        if (!entry_is_selected(entry.name, selected_count, selected_names)) {
            if (skip_bytes(archive, entry.stored_size) != 0) {
                diag_error("damaged data for '%s'", entry.name);
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
            diag_error("out of memory");
            free_entry(&entry);
            fclose(archive);
            free(seen);
            return -1;
        }

        if (io_file_exists(out_path)) {
            if (opts->overwrite_mode == PAK_OVERWRITE_SKIP) {
                log_item(opts, (int)i + 1, (int)count, "skip %s", entry.name);
                if (skip_bytes(archive, entry.stored_size) != 0) {
                    diag_error("damaged data for '%s'", entry.name);
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
                diag_error("refusing to overwrite '%s'", out_path);
                free(out_path);
                free_entry(&entry);
                fclose(archive);
                free(seen);
                return -1;
            }
        }

        if (io_make_parent_dirs(out_path) != 0) {
            diag_error("cannot create parent directory for '%s'", out_path);
            free(out_path);
            free_entry(&entry);
            fclose(archive);
            free(seen);
            return -1;
        }

        log_item(opts, (int)i + 1, (int)count, "extract %s", entry.name);
        out = fopen(out_path, "wb");
        if (out == NULL) {
            diag_error("%s: %s", out_path, strerror(errno));
            free(out_path);
            free_entry(&entry);
            fclose(archive);
            free(seen);
            return -1;
        }

        if (process_entry_data(archive, out, &entry, version, opts, 1) != 0) {
            diag_error("failed while extracting '%s'", entry.name);
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
        diag_error("%s: %s", archive_path, strerror(errno));
        return -1;
    }
    if (read_archive_header(archive, &version, &count) != 0 || io_file_size(archive_path, &archive_size) != 0) {
        diag_error("bad archive '%s'", archive_path);
        fclose(archive);
        return -1;
    }

    for (i = 0; i < count; i++) {
        struct pak_entry entry;

        if (read_entry_header(archive, version, &entry) != 0) {
            diag_error("damaged entry in '%s'", archive_path);
            fclose(archive);
            return -1;
        }
        unpacked_size += entry.size;
        stored_size += entry.stored_size;
        if (entry.flags != 0) {
            compressed++;
        }
        if (skip_bytes(archive, entry.stored_size) != 0) {
            diag_error("damaged data for '%s'", entry.name);
            free_entry(&entry);
            fclose(archive);
            return -1;
        }
        free_entry(&entry);
    }

    printf("%sarchive%s: %s%s%s\n", out_clr(PAK_CLR_CYAN), out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_BOLD), archive_path, out_clr(PAK_CLR_RESET));
    printf("%sformat%s: %sPAK%d%s\n", out_clr(PAK_CLR_CYAN), out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_BOLD), version, out_clr(PAK_CLR_RESET));
    printf("%sfiles%s: %s%u%s\n", out_clr(PAK_CLR_CYAN), out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_BOLD), count, out_clr(PAK_CLR_RESET));
    printf("%sarchive size%s: %s%llu bytes%s\n", out_clr(PAK_CLR_CYAN), out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_GREEN), (unsigned long long)archive_size, out_clr(PAK_CLR_RESET));
    printf("%sunpacked size%s: %s%llu bytes%s\n", out_clr(PAK_CLR_CYAN), out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_GREEN), (unsigned long long)unpacked_size, out_clr(PAK_CLR_RESET));
    printf("%sstored size%s: %s%llu bytes%s\n", out_clr(PAK_CLR_CYAN), out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_GREEN), (unsigned long long)stored_size, out_clr(PAK_CLR_RESET));
    printf("%scompressed files%s: %s%u%s\n", out_clr(PAK_CLR_CYAN), out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_BOLD), compressed, out_clr(PAK_CLR_RESET));
    if (unpacked_size > 0) {
        printf("%sstored ratio%s: %s%.1f%%%s\n", out_clr(PAK_CLR_CYAN), out_clr(PAK_CLR_RESET), out_clr(PAK_CLR_YELLOW), (double)stored_size * 100.0 / (double)unpacked_size, out_clr(PAK_CLR_RESET));
    }

    fclose(archive);
    return 0;
}

int pak_check(const char *archive_path, const struct pak_options *opts)
{
    FILE *archive;
    uint32_t count;
    uint32_t i;
    uint32_t compressed;
    uint64_t archive_size;
    uint64_t unpacked_size;
    uint64_t stored_size;
    long end_pos;
    int version;

    log_step(opts, "check %s", archive_path);
    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        return -1;
    }
    if (io_file_size(archive_path, &archive_size) != 0 || read_archive_header(archive, &version, &count) != 0) {
        diag_error("bad archive '%s'", archive_path);
        fclose(archive);
        return -1;
    }

    log_step(opts, "format PAK%d, %u file%s", version, count, count == 1 ? "" : "s");
    compressed = 0;
    unpacked_size = 0;
    stored_size = 0;
    for (i = 0; i < count; i++) {
        struct pak_entry entry;

        if (read_entry_header(archive, version, &entry) != 0) {
            diag_error("damaged entry in '%s'", archive_path);
            fclose(archive);
            return -1;
        }
        log_item(opts, (int)i + 1, (int)count, "check %s", entry.name);
        unpacked_size += entry.size;
        stored_size += entry.stored_size;
        if (entry.flags != 0) {
            compressed++;
        }
        if (process_entry_data(archive, NULL, &entry, version, opts, version >= 2) != 0) {
            free_entry(&entry);
            fclose(archive);
            return -1;
        }
        free_entry(&entry);
    }

    end_pos = ftell(archive);
    if (end_pos < 0 || (uint64_t)end_pos != archive_size) {
        if (end_pos >= 0 && archive_size > (uint64_t)end_pos) {
            diag_error("archive has %llu trailing byte%s", (unsigned long long)(archive_size - (uint64_t)end_pos), archive_size - (uint64_t)end_pos == 1 ? "" : "s");
        } else {
            diag_error("bad archive '%s'", archive_path);
        }
        fclose(archive);
        return -1;
    }

    fclose(archive);
    printf("ok: checked %u file%s, %llu stored bytes, %llu unpacked bytes", count, count == 1 ? "" : "s", (unsigned long long)stored_size, (unsigned long long)unpacked_size);
    if (compressed > 0) {
        printf(", %u compressed", compressed);
    }
    putchar('\n');
    return 0;
}
