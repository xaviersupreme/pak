#include "pak.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define is_stream_tty(stream) _isatty(_fileno(stream))
#else
#include <unistd.h>
#define is_stream_tty(stream) isatty(fileno(stream))
#endif

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

#ifdef _WIN32
static int enable_virtual_terminal(FILE *stream)
{
    HANDLE out = GetStdHandle(stream == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD mode;

    if (out == INVALID_HANDLE_VALUE || !GetConsoleMode(out, &mode)) {
        return 0;
    }
    if (!SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        return 0;
    }
    return 1;
}
#endif

static int color_enabled(FILE *stream)
{
    static int stdout_cached = -1;
    static int stderr_cached = -1;
    int *cached = stream == stderr ? &stderr_cached : &stdout_cached;
    const char *term;

    if (*cached != -1) {
        return *cached;
    }

    if (getenv("FORCE_COLOR") != NULL) {
#ifdef _WIN32
        enable_virtual_terminal(stream);
#endif
        *cached = 1;
        return *cached;
    }
    if (getenv("NO_COLOR") != NULL) {
        *cached = 0;
        return *cached;
    }
    if (!is_stream_tty(stream)) {
        *cached = 0;
        return *cached;
    }

    term = getenv("TERM");
    if (term != NULL && strcmp(term, "dumb") == 0) {
        *cached = 0;
        return *cached;
    }

#ifdef _WIN32
    if (!enable_virtual_terminal(stream)) {
        *cached = 0;
        return *cached;
    }
#endif

    *cached = 1;
    return *cached;
}

const char *pak_clr(FILE *stream, const char *code)
{
    return color_enabled(stream) ? code : "";
}

static const char *clr(const char *code)
{
    return pak_clr(stdout, code);
}

static int progress_open;

void log_finish_progress(void)
{
    if (progress_open) {
        putchar('\n');
        fflush(stdout);
        progress_open = 0;
    }
}

static void vlog_line(const struct pak_options *opts, const char *prefix, const char *fmt, va_list ap)
{
    if (opts != NULL && opts->quiet) {
        return;
    }

    log_finish_progress();
    fputs(clr(PAK_CLR_CYAN), stdout);
    fputs(prefix, stdout);
    fputs(clr(PAK_CLR_RESET), stdout);
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

    log_finish_progress();
    printf("  %s%d/%d%s  ", clr(PAK_CLR_DIM), index, total, clr(PAK_CLR_RESET));
    fputs(clr(PAK_CLR_BOLD), stdout);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputs(clr(PAK_CLR_RESET), stdout);
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

    printf("\r      %s[", clr(PAK_CLR_GREEN));
    for (i = 0; i < width; i++) {
        if (i == 0 || i == filled) {
            fputs(i < filled ? clr(PAK_CLR_GREEN) : clr(PAK_CLR_DIM), stdout);
        }
        putchar(i < filled ? '#' : '-');
    }
    printf("%s]%s %s%3d%%%s  %s%s%s/%s%s%s", clr(PAK_CLR_GREEN), clr(PAK_CLR_RESET), clr(percent == 100 ? PAK_CLR_GREEN : PAK_CLR_YELLOW), percent, clr(PAK_CLR_RESET), clr(PAK_CLR_BOLD), done_buf, clr(PAK_CLR_RESET), clr(PAK_CLR_DIM), total_buf, clr(PAK_CLR_RESET));
    progress_open = 1;

    if (force) {
        putchar('\n');
        progress_open = 0;
    }
    fflush(stdout);
}

void log_count_progress(const struct pak_options *opts, const char *name, uint64_t done, uint64_t total, int force)
{
    static char last_name[256];
    static int last_percent = -1;
    int width;
    int tenth;
    int percent;
    int filled;
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

    printf("\r      %s[", clr(PAK_CLR_GREEN));
    for (i = 0; i < width; i++) {
        if (i == 0 || i == filled) {
            fputs(i < filled ? clr(PAK_CLR_GREEN) : clr(PAK_CLR_DIM), stdout);
        }
        putchar(i < filled ? '#' : '-');
    }
    printf("%s]%s %s%3d%%%s  %s%llu%s/%s%llu%s", clr(PAK_CLR_GREEN), clr(PAK_CLR_RESET), clr(percent == 100 ? PAK_CLR_GREEN : PAK_CLR_YELLOW), percent, clr(PAK_CLR_RESET), clr(PAK_CLR_BOLD), (unsigned long long)done, clr(PAK_CLR_RESET), clr(PAK_CLR_DIM), (unsigned long long)total, clr(PAK_CLR_RESET));
    progress_open = 1;

    if (force) {
        putchar('\n');
        progress_open = 0;
    }
    fflush(stdout);
}
