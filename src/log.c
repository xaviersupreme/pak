#include "pak.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void format_size(uint64_t bytes, char *buf, size_t buf_size)
{
    if (bytes < 1024) {
        snprintf(buf, buf_size, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ull * 1024ull) {
        snprintf(buf, buf_size, "%.1f KiB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, buf_size, "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
    }
}

static void vlog_line(const struct pak_options *opts, const char *prefix, const char *fmt, va_list ap)
{
    if (opts == NULL || !opts->verbose) {
        return;
    }

    fputs(prefix, stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
}

void log_step(const struct pak_options *opts, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vlog_line(opts, "pak: ", fmt, ap);
    va_end(ap);
}

void log_item(const struct pak_options *opts, int index, int total, const char *fmt, ...)
{
    va_list ap;

    if (opts == NULL || !opts->verbose) {
        return;
    }

    printf("[%d/%d] ", index, total);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

void log_progress(const struct pak_options *opts, const char *name, uint64_t done, uint64_t total, int force)
{
    static char last_name[256];
    static int last_bucket = -1;
    int tenth;
    int bucket;
    char done_buf[32];
    char total_buf[32];

    if (opts == NULL || !opts->verbose) {
        return;
    }

    if (strncmp(last_name, name, sizeof(last_name)) != 0) {
        snprintf(last_name, sizeof(last_name), "%s", name);
        last_bucket = -1;
    }

    format_size(total, total_buf, sizeof(total_buf));

    if (force) {
        printf("    done %s\n", total_buf);
        fflush(stdout);
        return;
    }

    if (total == 0) {
        return;
    }

    tenth = (int)((done * 1000) / total);
    if (tenth > 1000) {
        tenth = 1000;
    }

    bucket = (tenth / 100) * 10;
    if (bucket <= 0 || bucket >= 100 || bucket == last_bucket) {
        return;
    }
    last_bucket = bucket;

    format_size(done, done_buf, sizeof(done_buf));
    printf("    %3d%%  %s / %s\n", bucket, done_buf, total_buf);
}