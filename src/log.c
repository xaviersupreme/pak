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
    if (opts != NULL && opts->quiet) {
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
    vlog_line(opts, "==> ", fmt, ap);
    va_end(ap);
}

void log_item(const struct pak_options *opts, int index, int total, const char *fmt, ...)
{
    va_list ap;

    if (opts != NULL && opts->quiet) {
        return;
    }

    printf("  %d/%d  ", index, total);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

void log_progress(const struct pak_options *opts, const char *name, uint64_t done, uint64_t total, int force)
{
    static char last_name[256];
    static int last_percent = -1;
    int width;
    int tenth;
    int percent;
    int filled;
    char done_buf[32];
    char total_buf[32];
    int i;

    if (opts != NULL && opts->quiet) {
        return;
    }

    if (strncmp(last_name, name, sizeof(last_name)) != 0) {
        snprintf(last_name, sizeof(last_name), "%s", name);
        last_percent = -1;
    }

    tenth = total == 0 ? 1000 : (int)((done * 1000) / total);
    if (tenth > 1000) {
        tenth = 1000;
    }
    percent = tenth / 10;

    if (!force && last_percent >= 0 && percent < last_percent + 5) {
        return;
    }
    if (!force && percent == last_percent) {
        return;
    }

    last_percent = percent;

    width = 28;
    filled = (tenth * width) / 1000;
    format_size(done, done_buf, sizeof(done_buf));
    format_size(total, total_buf, sizeof(total_buf));

    printf("\r      [");
    for (i = 0; i < width; i++) {
        putchar(i < filled ? '#' : '-');
    }
    printf("] %3d%%  %s/%s", percent, done_buf, total_buf);

    if (force) {
        putchar('\n');
    }
    fflush(stdout);
}
