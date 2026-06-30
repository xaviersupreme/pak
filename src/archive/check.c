#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static int stdin_is_tty(void)
{
    DWORD mode;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);

    return in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode) != 0;
}
#else
#include <unistd.h>
#define stdin_is_tty() isatty(fileno(stdin))
#endif
struct check_report {
    struct old_entry_ref *entries;
    struct check_issue *issues;
    uint32_t entry_count;
    uint32_t entry_capacity;
    uint32_t issue_count;
    uint32_t issue_capacity;
    uint32_t expected_count;
    uint32_t damaged_count;
    uint32_t salvaged_count;
    uint32_t dropped_count;
    uint32_t compressed_count;
    uint64_t unpacked_size;
    uint64_t stored_size;
    int version;
};

struct check_issue {
    uint32_t index;
    uint64_t offset;
    char target[96];
    char reason[96];
    char action[48];
};

static const char *check_out_clr(const char *code)
{
    return pak_clr(stdout, code);
}

static const char *check_err_clr(const char *code)
{
    return pak_clr(stderr, code);
}

static const char *check_stream_clr(FILE *stream, const char *code)
{
    return pak_clr(stream, code);
}

static void format_check_size(char *buf, size_t buf_size, uint64_t bytes)
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

static void copy_report_text(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    len = strlen(src);
    if (len < dst_size) {
        memcpy(dst, src, len + 1);
        return;
    }

    if (dst_size <= 4) {
        memcpy(dst, "...", dst_size - 1);
        dst[dst_size - 1] = '\0';
        return;
    }

    memcpy(dst, src, dst_size - 4);
    memcpy(dst + dst_size - 4, "...", 4);
}

static int check_report_add_issue(struct check_report *report, uint32_t index, uint64_t offset, const char *target, const char *reason, const char *action)
{
    struct check_issue *grown;
    uint32_t new_capacity;
    struct check_issue *issue;

    if (report->issue_count == report->issue_capacity) {
        new_capacity = report->issue_capacity == 0 ? 8 : report->issue_capacity * 2;
        if (new_capacity < report->issue_capacity) {
            return -1;
        }
        grown = realloc(report->issues, (size_t)new_capacity * sizeof(*report->issues));
        if (grown == NULL) {
            return -1;
        }
        report->issues = grown;
        report->issue_capacity = new_capacity;
    }

    issue = &report->issues[report->issue_count++];
    issue->index = index;
    issue->offset = offset;
    copy_report_text(issue->target, sizeof(issue->target), target);
    copy_report_text(issue->reason, sizeof(issue->reason), reason);
    copy_report_text(issue->action, sizeof(issue->action), action);
    return 0;
}


static int copy_recovered_bytes(FILE *in, FILE *out, uint64_t wanted, uint64_t available, uint32_t *crc, uint64_t *wrote, const char *name, const struct pak_options *opts)
{
    unsigned char buf[COPY_BUF_SIZE];
    uint64_t left;
    uint64_t total;

    left = wanted < available ? wanted : available;
    total = 0;
    log_step(opts, "recover stored %s: %llu of %llu bytes available", name, (unsigned long long)left, (unsigned long long)wanted);
    while (left > 0) {
        size_t want = left > COPY_BUF_SIZE ? COPY_BUF_SIZE : (size_t)left;
        size_t got = fread(buf, 1, want, in);

        if (got == 0) {
            if (ferror(in)) {
                return -1;
            }
            break;
        }
        if (fwrite(buf, 1, got, out) != got) {
            return -1;
        }
        *crc = crc32_update(*crc, buf, got);
        left -= got;
        total += got;
        log_progress(opts, name, total, wanted, total == wanted);
    }

    *wrote = total;
    return 0;
}

static int write_padding_bytes(FILE *out, uint64_t count, unsigned char value, uint32_t *crc)
{
    unsigned char buf[COPY_BUF_SIZE];
    uint64_t left = count;

    memset(buf, value, sizeof(buf));
    while (left > 0) {
        size_t take = left > COPY_BUF_SIZE ? COPY_BUF_SIZE : (size_t)left;

        if (fwrite(buf, 1, take, out) != take) {
            return -1;
        }
        *crc = crc32_update(*crc, buf, take);
        left -= take;
    }
    return 0;
}

static int copy_temp_payload(FILE *tmp, FILE *out)
{
    unsigned char buf[COPY_BUF_SIZE];

    rewind(tmp);
    for (;;) {
        size_t got = fread(buf, 1, sizeof(buf), tmp);

        if (got == 0) {
            return ferror(tmp) ? -1 : 0;
        }
        if (fwrite(buf, 1, got, out) != got) {
            return -1;
        }
    }
}

static const char *padding_name(unsigned char value)
{
    return value == ' ' ? "spaces" : "zero bytes";
}

static int name_looks_text(const char *name)
{
    static const char *text_exts[] = {
        "c",
        "cfg",
        "cpp",
        "css",
        "csv",
        "h",
        "hpp",
        "html",
        "ini",
        "js",
        "json",
        "log",
        "lua",
        "md",
        "txt",
        "xml"
    };
    const char *ext = strrchr(name, '.');
    size_t i;

    if (ext == NULL) {
        return 0;
    }
    ext++;
    for (i = 0; i < sizeof(text_exts) / sizeof(text_exts[0]); i++) {
        if (same_archive_name(ext, text_exts[i])) {
            return 1;
        }
    }
    return 0;
}

static int recover_entry_payload(FILE *archive, FILE *tmp, const struct old_entry_ref *ref, int version, uint32_t *crc, uint64_t *wrote, const struct pak_options *opts)
{
    uint64_t available = ref->available_size;

    (void)version;
    if (pak_file_seek(archive, ref->data_offset) != 0) {
        return -1;
    }
    if (available > ref->entry.stored_size) {
        available = ref->entry.stored_size;
    }

    if ((ref->entry.flags & PAK_FLAG_DEFLATE) != 0) {
        return deflate_recover(archive, tmp, available, ref->entry.size, crc, wrote, ref->entry.name, opts);
    }
    if ((ref->entry.flags & PAK_FLAG_RLE) != 0) {
        return rle_recover(archive, tmp, available, ref->entry.size, crc, wrote, ref->entry.name, opts);
    }
    return copy_recovered_bytes(archive, tmp, ref->entry.size, available, crc, wrote, ref->entry.name, opts);
}

static int write_salvaged_entry(FILE *archive, FILE *out, const struct old_entry_ref *ref, int version, const struct pak_options *opts)
{
    struct pak_entry entry;
    FILE *tmp;
    uint32_t crc;
    uint64_t wrote;
    int binary;
    unsigned char pad;

    tmp = tmpfile();
    if (tmp == NULL) {
        return -1;
    }

    crc = crc32_start();
    wrote = 0;
    if (recover_entry_payload(archive, tmp, ref, version, &crc, &wrote, opts) != 0 || fflush(tmp) != 0) {
        fclose(tmp);
        return -1;
    }

    if (wrote < ref->entry.size) {
        binary = name_looks_text(ref->entry.name) ? 0 : 1;
        if (wrote > 0 && file_looks_binary(tmp, &binary) != 0) {
            fclose(tmp);
            return -1;
        }
        pad = binary ? 0 : ' ';
        log_step(opts, "pad %s: %llu byte%s with %s", ref->entry.name, (unsigned long long)(ref->entry.size - wrote), ref->entry.size - wrote == 1 ? "" : "s", padding_name(pad));
        if (pak_file_seek_end(tmp) != 0 || write_padding_bytes(tmp, ref->entry.size - wrote, pad, &crc) != 0 || fflush(tmp) != 0) {
            fclose(tmp);
            return -1;
        }
    }

    entry = ref->entry;
    entry.flags = 0;
    entry.stored_size = entry.size;
    entry.checksum = crc32_finish(crc);
    if (write_entry_header(out, &entry) != 0 || copy_temp_payload(tmp, out) != 0) {
        fclose(tmp);
        return -1;
    }

    fclose(tmp);
    return 0;
}

static void check_report_init(struct check_report *report)
{
    memset(report, 0, sizeof(*report));
}

static void check_report_free(struct check_report *report)
{
    free_old_entry_refs(report->entries, report->entry_count);
    free(report->issues);
    memset(report, 0, sizeof(*report));
}

static int checked_add_u64(uint64_t *value, uint64_t add)
{
    if (UINT64_MAX - *value < add) {
        return -1;
    }
    *value += add;
    return 0;
}

static int seek_archive(FILE *archive, uint64_t offset)
{
    return pak_file_seek(archive, offset);
}

static int entry_data_end(uint64_t data_offset, uint64_t stored_size, uint64_t archive_size, uint64_t *end)
{
    if (data_offset > archive_size || stored_size > archive_size - data_offset) {
        return -1;
    }
    *end = data_offset + stored_size;
    return 0;
}

static int check_report_add_entry(struct check_report *report, struct pak_entry *entry, uint64_t data_offset, int repair_mode, uint64_t available_size)
{
    struct old_entry_ref *grown;
    uint32_t new_capacity;

    if (checked_add_u64(&report->unpacked_size, entry->size) != 0 || checked_add_u64(&report->stored_size, repair_mode == REPAIR_SALVAGE ? entry->size : entry->stored_size) != 0) {
        return -1;
    }
    if (report->entry_count == UINT32_MAX) {
        return -1;
    }
    if (report->entry_count == report->entry_capacity) {
        new_capacity = report->entry_capacity == 0 ? 16 : report->entry_capacity * 2;
        if (new_capacity < report->entry_capacity) {
            return -1;
        }
        grown = realloc(report->entries, (size_t)new_capacity * sizeof(*report->entries));
        if (grown == NULL) {
            return -1;
        }
        report->entries = grown;
        report->entry_capacity = new_capacity;
    }

    if (entry->flags != 0) {
        report->compressed_count++;
    }
    report->entries[report->entry_count].entry = *entry;
    report->entries[report->entry_count].data_offset = data_offset;
    report->entries[report->entry_count].repair_mode = repair_mode;
    report->entries[report->entry_count].available_size = available_size;
    report->entry_count++;
    if (repair_mode == REPAIR_SALVAGE) {
        report->salvaged_count++;
    }
    memset(entry, 0, sizeof(*entry));
    return 0;
}

static int validate_entry_data_quiet(FILE *archive, const struct pak_entry *entry, int version, const struct pak_options *opts)
{
    struct pak_options quiet_opts;

    quiet_opts = *opts;
    quiet_opts.quiet = 1;
    return process_entry_data_impl(archive, NULL, entry, version, &quiet_opts, version >= 2, 0);
}

static int read_valid_entry_at(FILE *archive, int version, uint64_t offset, uint64_t archive_size, const struct pak_options *opts, struct old_entry_ref *ref, uint64_t *end)
{
    struct pak_entry entry;
    uint64_t data_offset;

    memset(ref, 0, sizeof(*ref));
    clearerr(archive);
    if (seek_archive(archive, offset) != 0 || read_entry_header(archive, version, &entry) != 0) {
        return -1;
    }

    if (pak_file_tell(archive, &data_offset) != 0 || entry_data_end(data_offset, entry.stored_size, archive_size, end) != 0) {
        free_entry(&entry);
        return -1;
    }

    if (seek_archive(archive, data_offset) != 0 || validate_entry_data_quiet(archive, &entry, version, opts) != 0 || seek_archive(archive, *end) != 0) {
        free_entry(&entry);
        return -1;
    }

    ref->entry = entry;
    ref->data_offset = data_offset;
    ref->repair_mode = REPAIR_COPY;
    ref->available_size = entry.stored_size;
    return 0;
}

static int find_valid_entry_from(FILE *archive, int version, uint64_t start, uint64_t archive_size, const struct pak_options *opts, struct old_entry_ref *ref, uint64_t *header_pos, uint64_t *end)
{
    uint64_t pos;

    if (version < 2 || start >= archive_size) {
        return 0;
    }

    for (pos = start; pos + 29 <= archive_size; pos++) {
        if (read_valid_entry_at(archive, version, pos, archive_size, opts, ref, end) == 0) {
            *header_pos = pos;
            return 1;
        }
    }

    return 0;
}

static int check_add_resynced_entry(struct check_report *report, struct old_entry_ref *ref, uint32_t index, uint64_t found_pos, const struct pak_options *opts)
{
    log_step(opts, "resynced at offset %llu", (unsigned long long)found_pos);
    if (report->expected_count > 0) {
        log_item(opts, (int)index, (int)report->expected_count, "check %s", ref->entry.name);
    } else {
        log_step(opts, "check %s", ref->entry.name);
    }
    if (check_report_add_entry(report, &ref->entry, ref->data_offset, REPAIR_COPY, ref->entry.stored_size) != 0) {
        free_entry(&ref->entry);
        diag_error("out of memory");
        return -1;
    }
    return 0;
}

static int check_resync(FILE *archive, struct check_report *report, uint64_t archive_size, uint64_t start, uint32_t index, const struct pak_options *opts, uint64_t *match_pos, uint64_t *next_pos)
{
    uint64_t found_pos;
    uint64_t end;
    struct old_entry_ref ref;
    int found;

    found = find_valid_entry_from(archive, report->version, start, archive_size, opts, &ref, &found_pos, &end);
    if (found <= 0) {
        return found;
    }

    if (check_add_resynced_entry(report, &ref, index, found_pos, opts) != 0) {
        return -1;
    }
    *match_pos = found_pos;
    *next_pos = end;
    return 1;
}

static int check_damage_entry(struct check_report *report, uint32_t index, uint64_t offset, const char *archive_path, const char *reason)
{
    report->damaged_count++;
    report->dropped_count++;
    return check_report_add_issue(report, index, offset, archive_path, reason, "drop entry");
}

static int check_damage_data(struct check_report *report, uint32_t index, const struct pak_entry *entry, uint64_t offset, const char *reason)
{
    report->damaged_count++;
    return check_report_add_issue(report, index, offset, entry->name, reason, "salvage if readable");
}

static int check_damage_gap(struct check_report *report, uint64_t offset, uint64_t size, const char *target)
{
    char reason[96];

    report->damaged_count++;
    report->dropped_count++;
    snprintf(reason, sizeof(reason), "%llu unreadable byte%s", (unsigned long long)size, size == 1 ? "" : "s");
    return check_report_add_issue(report, 0, offset, target, reason, "drop bytes");
}

static int check_report_add_salvage_entry(struct check_report *report, struct pak_entry *entry, uint64_t data_offset, uint64_t archive_size, uint64_t limit)
{
    uint64_t available;

    if (report->version < 2 || data_offset >= archive_size) {
        free_entry(entry);
        report->dropped_count++;
        return 0;
    }

    available = archive_size - data_offset;
    if (limit > data_offset && limit - data_offset < available) {
        available = limit - data_offset;
    }
    if (available > entry->stored_size) {
        available = entry->stored_size;
    }
    if (available == 0 && entry->size > 0) {
        free_entry(entry);
        report->dropped_count++;
        return 0;
    }

    return check_report_add_entry(report, entry, data_offset, REPAIR_SALVAGE, available);
}

static int check_scan_entries(FILE *archive, struct check_report *report, uint64_t archive_size, const struct pak_options *opts)
{
    uint64_t pos = archive_size > 8 ? 8 : 0;

    report->version = 2;
    log_step(opts, "scan for recoverable PAK2 entries");
    while (pos + 29 <= archive_size) {
        struct old_entry_ref ref;
        uint64_t found_pos;
        uint64_t end;
        int found;

        found = find_valid_entry_from(archive, report->version, pos, archive_size, opts, &ref, &found_pos, &end);
        if (found < 0) {
            return -1;
        }
        if (found == 0) {
            if (pos < archive_size && check_damage_gap(report, pos, archive_size - pos, "archive gap") != 0) {
                return -1;
            }
            break;
        }
        if (found_pos > pos && check_damage_gap(report, pos, found_pos - pos, "archive gap") != 0) {
            free_entry(&ref.entry);
            return -1;
        }
        log_step(opts, "recovered %s at offset %llu", ref.entry.name, (unsigned long long)found_pos);
        if (check_report_add_entry(report, &ref.entry, ref.data_offset, REPAIR_COPY, ref.entry.stored_size) != 0) {
            free_entry(&ref.entry);
            diag_error("out of memory");
            return -1;
        }
        pos = end;
    }
    return 0;
}

static int check_archive_collect(const char *archive_path, const struct pak_options *opts, struct check_report *report)
{
    FILE *archive;
    uint64_t archive_size;
    uint64_t pos;
    uint32_t i;

    check_report_init(report);
    log_step(opts, "check %s", archive_path);

    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        return -1;
    }
    if (io_file_size(archive_path, &archive_size) != 0) {
        diag_error("bad archive '%s'", archive_path);
        fclose(archive);
        return -1;
    }
    if (read_archive_header(archive, &report->version, &report->expected_count) != 0) {
        report->damaged_count++;
        if (check_report_add_issue(report, 0, 0, archive_path, "archive header is unreadable", "scan for entries") != 0) {
            fclose(archive);
            return -1;
        }
        if (check_scan_entries(archive, report, archive_size, opts) != 0) {
            fclose(archive);
            return -1;
        }
        fclose(archive);
        return 0;
    }

    log_step(opts, "format PAK%d, %u file%s", report->version, report->expected_count, report->expected_count == 1 ? "" : "s");
    for (i = 0; i < report->expected_count; i++) {
        struct pak_entry entry;
        uint64_t header_pos;
        uint64_t data_offset;
        uint64_t data_end;
        uint64_t match_pos;
        uint64_t next_pos;
        int recovered;

        match_pos = 0;
        next_pos = 0;
        if (pak_file_tell(archive, &header_pos) != 0 || header_pos >= archive_size) {
            if (report->entry_count + report->damaged_count < report->expected_count) {
                if (check_damage_entry(report, i + 1, 0, archive_path, "entry header is missing") != 0) {
                    fclose(archive);
                    return -1;
                }
            }
            break;
        }

        if (read_entry_header(archive, report->version, &entry) != 0) {
            if (check_damage_entry(report, i + 1, header_pos, archive_path, "entry header is unreadable") != 0) {
                fclose(archive);
                return -1;
            }
            recovered = check_resync(archive, report, archive_size, header_pos + 1, i + 1, opts, &match_pos, &next_pos);
            if (recovered < 0) {
                fclose(archive);
                return -1;
            }
            if (recovered == 0 || seek_archive(archive, next_pos) != 0) {
                break;
            }
            continue;
        }

        if (pak_file_tell(archive, &data_offset) != 0) {
            if (check_damage_data(report, i + 1, &entry, header_pos, "entry data offset is unreadable") != 0) {
                free_entry(&entry);
                fclose(archive);
                return -1;
            }
            recovered = check_resync(archive, report, archive_size, header_pos + 1, i + 1, opts, &match_pos, &next_pos);
            free_entry(&entry);
            if (recovered < 0) {
                fclose(archive);
                return -1;
            }
            if (recovered == 0 || seek_archive(archive, next_pos) != 0) {
                break;
            }
            continue;
        }

        if (entry_data_end(data_offset, entry.stored_size, archive_size, &data_end) != 0) {
            struct old_entry_ref sync_ref;
            uint64_t limit;

            memset(&sync_ref, 0, sizeof(sync_ref));
            limit = archive_size;
            if (check_damage_data(report, i + 1, &entry, data_offset, "entry data is truncated") != 0) {
                free_entry(&entry);
                fclose(archive);
                return -1;
            }
            recovered = find_valid_entry_from(archive, report->version, header_pos + 1, archive_size, opts, &sync_ref, &match_pos, &next_pos);
            if (recovered < 0) {
                free_entry(&entry);
                fclose(archive);
                return -1;
            }
            if (recovered > 0) {
                limit = match_pos;
            }
            if (check_report_add_salvage_entry(report, &entry, data_offset, archive_size, limit) != 0) {
                free_entry(&entry);
                free_entry(&sync_ref.entry);
                diag_error("out of memory");
                fclose(archive);
                return -1;
            }
            if (recovered > 0) {
                if (check_add_resynced_entry(report, &sync_ref, i + 1, match_pos, opts) != 0) {
                    fclose(archive);
                    return -1;
                }
                if (seek_archive(archive, next_pos) != 0) {
                    fclose(archive);
                    return -1;
                }
                continue;
            }
            if (seek_archive(archive, archive_size) != 0) {
                fclose(archive);
                return -1;
            }
            break;
        }

        log_item(opts, (int)i + 1, (int)report->expected_count, "check %s", entry.name);
        if (seek_archive(archive, data_offset) != 0 || process_entry_data_impl(archive, NULL, &entry, report->version, opts, report->version >= 2, 0) != 0) {
            struct old_entry_ref sync_ref;
            uint64_t limit;

            memset(&sync_ref, 0, sizeof(sync_ref));
            limit = data_end;
            if (check_damage_data(report, i + 1, &entry, data_offset, "entry failed decompression or crc") != 0) {
                free_entry(&entry);
                fclose(archive);
                return -1;
            }
            recovered = find_valid_entry_from(archive, report->version, data_offset + 1, archive_size, opts, &sync_ref, &match_pos, &next_pos);
            if (recovered == 0 && data_end < archive_size) {
                recovered = find_valid_entry_from(archive, report->version, data_end, archive_size, opts, &sync_ref, &match_pos, &next_pos);
            }
            if (recovered < 0) {
                free_entry(&entry);
                fclose(archive);
                return -1;
            }
            if (recovered > 0 && match_pos > data_offset && match_pos < limit) {
                limit = match_pos;
            }
            if (check_report_add_salvage_entry(report, &entry, data_offset, archive_size, limit) != 0) {
                free_entry(&entry);
                free_entry(&sync_ref.entry);
                diag_error("out of memory");
                fclose(archive);
                return -1;
            }
            if (recovered > 0) {
                if (check_add_resynced_entry(report, &sync_ref, i + 1, match_pos, opts) != 0) {
                    fclose(archive);
                    return -1;
                }
                if (seek_archive(archive, next_pos) != 0) {
                    fclose(archive);
                    return -1;
                }
                continue;
            }
            if (seek_archive(archive, data_end) != 0) {
                fclose(archive);
                return -1;
            }
            continue;
        }

        if (seek_archive(archive, data_end) != 0) {
            free_entry(&entry);
            fclose(archive);
            return -1;
        }
        if (check_report_add_entry(report, &entry, data_offset, REPAIR_COPY, entry.stored_size) != 0) {
            free_entry(&entry);
            diag_error("out of memory");
            fclose(archive);
            return -1;
        }
    }

    {
        uint64_t end_pos;

        if (pak_file_tell(archive, &end_pos) != 0) {
            fclose(archive);
            return -1;
        }
        pos = end_pos;
    }
    if (pos < archive_size) {
        report->damaged_count++;
        if (check_report_add_issue(report, 0, pos, "archive end", "trailing junk after last entry", "strip bytes") != 0) {
            fclose(archive);
            return -1;
        }
    } else if (pos > archive_size) {
        report->damaged_count++;
        if (check_report_add_issue(report, 0, archive_size, archive_path, "entry sizes point past end of file", "repair layout") != 0) {
            fclose(archive);
            return -1;
        }
    }

    fclose(archive);
    return 0;
}

static void print_check_ok(const struct check_report *report)
{
    char stored[32];
    char unpacked[32];

    format_check_size(stored, sizeof(stored), report->stored_size);
    format_check_size(unpacked, sizeof(unpacked), report->unpacked_size);
    printf("%sok:%s checked %s%u%s file%s, %s%s%s stored, %s%s%s unpacked", check_out_clr(PAK_CLR_BOLD PAK_CLR_GREEN), check_out_clr(PAK_CLR_RESET), check_out_clr(PAK_CLR_BOLD), report->entry_count, check_out_clr(PAK_CLR_RESET), report->entry_count == 1 ? "" : "s", check_out_clr(PAK_CLR_GREEN), stored, check_out_clr(PAK_CLR_RESET), check_out_clr(PAK_CLR_GREEN), unpacked, check_out_clr(PAK_CLR_RESET));
    if (report->compressed_count > 0) {
        printf(", %u compressed", report->compressed_count);
    }
    putchar('\n');
}

static void print_check_report_line(const char *label, const char *value, const char *color)
{
    printf("  %s%-8s%s %s%s%s\n", check_out_clr(PAK_CLR_CYAN), label, check_out_clr(PAK_CLR_RESET), check_out_clr(color), value, check_out_clr(PAK_CLR_RESET));
}

static void print_repair_detail_line(FILE *stream, const char *verb, const char *name, const char *note, const char *color)
{
    fprintf(stream, "  %s%-7s%s %s%s%s", check_stream_clr(stream, color), verb, check_stream_clr(stream, PAK_CLR_RESET), check_stream_clr(stream, PAK_CLR_BOLD), name, check_stream_clr(stream, PAK_CLR_RESET));
    if (note != NULL && note[0] != '\0') {
        fprintf(stream, "  %s%s%s", check_stream_clr(stream, PAK_CLR_DIM), note, check_stream_clr(stream, PAK_CLR_RESET));
    }
    fputc('\n', stream);
}

static void print_repair_details(FILE *stream, const struct check_report *report)
{
    uint32_t i;

    fprintf(stream, "\n%srepair details%s\n", check_stream_clr(stream, PAK_CLR_BOLD PAK_CLR_CYAN), check_stream_clr(stream, PAK_CLR_RESET));
    fprintf(stream, "--------------\n");

    for (i = 0; i < report->entry_count; i++) {
        const struct old_entry_ref *ref = &report->entries[i];

        if (ref->repair_mode == REPAIR_SALVAGE) {
            print_repair_detail_line(stream, "salvage", ref->entry.name, "recovered bytes will be padded if needed", PAK_CLR_YELLOW);
        } else {
            print_repair_detail_line(stream, "keep", ref->entry.name, "validated", PAK_CLR_GREEN);
        }
    }

    for (i = 0; i < report->issue_count; i++) {
        const struct check_issue *issue = &report->issues[i];

        if (strncmp(issue->action, "drop", 4) == 0) {
            print_repair_detail_line(stream, "drop", issue->target, issue->reason, PAK_CLR_RED);
        } else if (strncmp(issue->action, "strip", 5) == 0) {
            print_repair_detail_line(stream, "strip", issue->target, issue->reason, PAK_CLR_YELLOW);
        }
    }
}

static void print_check_damage_report(const struct check_report *report, const char *out_path)
{
    uint32_t clean_count;
    char stored[32];
    char unpacked[32];
    char value[128];
    uint32_t i;

    if (report->issue_count == 0) {
        return;
    }

    clean_count = report->entry_count - report->salvaged_count;
    format_check_size(stored, sizeof(stored), report->stored_size);
    format_check_size(unpacked, sizeof(unpacked), report->unpacked_size);

    log_finish_progress();
    printf("%scheck report%s\n", check_out_clr(PAK_CLR_BOLD PAK_CLR_CYAN), check_out_clr(PAK_CLR_RESET));
    printf("------------\n");
    snprintf(value, sizeof(value), "%u expected, %u readable", report->expected_count, report->entry_count);
    print_check_report_line("entries", value, PAK_CLR_BOLD);
    snprintf(value, sizeof(value), "%u issue%s", report->damaged_count, report->damaged_count == 1 ? "" : "s");
    print_check_report_line("damage", value, PAK_CLR_RED);
    snprintf(value, sizeof(value), "%s stored, %s unpacked", stored, unpacked);
    print_check_report_line("size", value, PAK_CLR_GREEN);

    printf("\n%sdamage%s\n", check_out_clr(PAK_CLR_BOLD PAK_CLR_RED), check_out_clr(PAK_CLR_RESET));
    for (i = 0; i < report->issue_count; i++) {
        const struct check_issue *issue = &report->issues[i];

        if (issue->index > 0 && report->expected_count > 0) {
            printf("  %s%u/%u%s  %s%s%s\n", check_out_clr(PAK_CLR_DIM), issue->index, report->expected_count, check_out_clr(PAK_CLR_RESET), check_out_clr(PAK_CLR_BOLD), issue->target, check_out_clr(PAK_CLR_RESET));
        } else if (issue->index > 0) {
            printf("  %s%u%s  %s%s%s\n", check_out_clr(PAK_CLR_DIM), issue->index, check_out_clr(PAK_CLR_RESET), check_out_clr(PAK_CLR_BOLD), issue->target, check_out_clr(PAK_CLR_RESET));
        } else {
            printf("  %s%s%s\n", check_out_clr(PAK_CLR_BOLD), issue->target, check_out_clr(PAK_CLR_RESET));
        }
        printf("      %sreason%s  %s at offset %llu\n", check_out_clr(PAK_CLR_CYAN), check_out_clr(PAK_CLR_RESET), issue->reason, (unsigned long long)issue->offset);
        printf("      %srepair%s  %s\n", check_out_clr(PAK_CLR_CYAN), check_out_clr(PAK_CLR_RESET), issue->action);
    }

    printf("\n%srepair plan%s\n", check_out_clr(PAK_CLR_BOLD PAK_CLR_CYAN), check_out_clr(PAK_CLR_RESET));
    snprintf(value, sizeof(value), "%u clean, %u salvaged, %u dropped", clean_count, report->salvaged_count, report->dropped_count);
    print_check_report_line("entries", value, PAK_CLR_BOLD);
    if (report->entry_count == 0) {
        print_check_report_line("output", "none", PAK_CLR_RED);
    } else {
        print_check_report_line("output", out_path, PAK_CLR_GREEN);
        print_check_report_line("details", "press d at the repair prompt", PAK_CLR_YELLOW);
    }
}

static int archive_ascii_lower(int ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 'a';
    }
    return ch;
}

static int archive_has_pak_extension(const char *path)
{
    size_t len = strlen(path);
    const char *ext;

    if (len < 4) {
        return 0;
    }
    ext = path + len - 4;
    return archive_ascii_lower(ext[0]) == '.' && archive_ascii_lower(ext[1]) == 'p' && archive_ascii_lower(ext[2]) == 'a' && archive_ascii_lower(ext[3]) == 'k';
}

static char *make_repaired_archive_path(const char *archive_path)
{
    char *out;
    size_t len = strlen(archive_path);
    size_t base_len = archive_has_pak_extension(archive_path) ? len - 4 : len;

    out = malloc(base_len + sizeof(".repaired.pak"));
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, archive_path, base_len);
    memcpy(out + base_len, ".repaired.pak", sizeof(".repaired.pak"));
    return out;
}

static int prompt_repair(const char *out_path, const struct check_report *report)
{
    char answer[32];
    unsigned char ch;

    if (!stdin_is_tty()) {
        printf("\n%s%-8s%s rerun in a terminal to attempt repair\n", check_out_clr(PAK_CLR_YELLOW), "hint", check_out_clr(PAK_CLR_RESET));
        fflush(stdout);
        return 0;
    }

    for (;;) {
        fprintf(stderr, "%srepair:%s write recovered archive to %s'%s'%s? [Y/n/d] ", check_err_clr(PAK_CLR_BOLD PAK_CLR_CYAN), check_err_clr(PAK_CLR_RESET), check_err_clr(PAK_CLR_GREEN), out_path, check_err_clr(PAK_CLR_RESET));
        fflush(stderr);
        if (fgets(answer, sizeof(answer), stdin) == NULL) {
            return 0;
        }

        ch = (unsigned char)answer[0];
        if (ch == '\n' || ch == '\0' || ch == 'y' || ch == 'Y') {
            return 1;
        }
        if (ch == 'n' || ch == 'N') {
            fprintf(stderr, "%srepair:%s aborted\n", check_err_clr(PAK_CLR_DIM), check_err_clr(PAK_CLR_RESET));
            return 0;
        }
        if (ch == 'd' || ch == 'D') {
            print_repair_details(stderr, report);
            continue;
        }
        fprintf(stderr, "%shint:%s choose y to repair, n to abort, or d for details\n", check_err_clr(PAK_CLR_YELLOW), check_err_clr(PAK_CLR_RESET));
    }
}

static int write_repaired_archive(const char *archive_path, const char *out_path, const struct check_report *report, const struct pak_options *opts)
{
    FILE *archive;
    FILE *out;
    uint32_t i;
    int rc;

    if (report->entry_count == 0) {
        diag_error("no recoverable entries found");
        return -1;
    }
    if (io_file_exists(out_path)) {
        diag_error("%s already exists", out_path);
        diag_hint("remove it or rename it before repairing");
        return -1;
    }

    archive = fopen(archive_path, "rb");
    if (archive == NULL) {
        diag_error("%s: %s", archive_path, strerror(errno));
        return -1;
    }
    out = fopen(out_path, "wb");
    if (out == NULL) {
        diag_error("%s: %s", out_path, strerror(errno));
        fclose(archive);
        return -1;
    }

    rc = -1;
    log_step(opts, "repair %s -> %s", archive_path, out_path);
    if (write_archive_header(out, report->entry_count) != 0) {
        diag_error("failed to write archive header");
        goto done;
    }
    for (i = 0; i < report->entry_count; i++) {
        if (report->entries[i].repair_mode == REPAIR_SALVAGE) {
            log_item(opts, (int)i + 1, (int)report->entry_count, "salvage %s", report->entries[i].entry.name);
            if (write_salvaged_entry(archive, out, &report->entries[i], report->version, opts) != 0) {
                diag_error("failed while salvaging '%s'", report->entries[i].entry.name);
                goto done;
            }
            continue;
        }
        log_item(opts, (int)i + 1, (int)report->entry_count, "write clean %s", report->entries[i].entry.name);
        if (copy_kept_entry(archive, out, &report->entries[i], report->version) != 0) {
            diag_error("failed while writing '%s'", report->entries[i].entry.name);
            goto done;
        }
    }
    rc = 0;

done:
    if (fclose(out) != 0) {
        diag_error("failed to finish %s", out_path);
        rc = -1;
    }
    if (fclose(archive) != 0) {
        diag_error("failed to close %s", archive_path);
        rc = -1;
    }
    if (rc != 0) {
        remove(out_path);
    }
    return rc;
}

int pak_check(const char *archive_path, const struct pak_options *opts)
{
    struct check_report report;
    struct check_report repaired_report;
    char *out_path;
    int rc;

    if (check_archive_collect(archive_path, opts, &report) != 0) {
        check_report_free(&report);
        return -1;
    }
    if (report.damaged_count == 0) {
        print_check_ok(&report);
        check_report_free(&report);
        return 0;
    }

    out_path = make_repaired_archive_path(archive_path);
    if (out_path == NULL) {
        check_report_free(&report);
        diag_error("out of memory");
        return -1;
    }
    print_check_damage_report(&report, out_path);
    if (report.entry_count == 0) {
        free(out_path);
        check_report_free(&report);
        diag_error("no recoverable entries found");
        return -1;
    }
    if (!prompt_repair(out_path, &report)) {
        free(out_path);
        check_report_free(&report);
        return -1;
    }

    rc = -1;
    if (write_repaired_archive(archive_path, out_path, &report, opts) == 0) {
        if (check_archive_collect(out_path, opts, &repaired_report) == 0 && repaired_report.damaged_count == 0) {
            print_check_ok(&repaired_report);
            printf("%srepaired:%s wrote %s%s%s (%u clean, %u salvaged, %u dropped)\n", check_out_clr(PAK_CLR_BOLD PAK_CLR_GREEN), check_out_clr(PAK_CLR_RESET), check_out_clr(PAK_CLR_GREEN), out_path, check_out_clr(PAK_CLR_RESET), report.entry_count - report.salvaged_count, report.salvaged_count, report.dropped_count);
            rc = 0;
        } else {
            diag_error("repaired archive is still damaged");
        }
        check_report_free(&repaired_report);
    }

    free(out_path);
    check_report_free(&report);
    return rc;
}
