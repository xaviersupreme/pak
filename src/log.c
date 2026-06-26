#include "pak.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define is_stdout_tty() _isatty(_fileno(stdout))
#else
#include <unistd.h>
#define is_stdout_tty() isatty(fileno(stdout))
#endif

#define CLR_RESET "\033[0m"
#define CLR_DIM "\033[2m"
#define CLR_CYAN "\033[36m"
#define CLR_GREEN "\033[32m"
#define CLR_YELLOW "\033[33m"
#define CLR_BOLD "\033[1m"

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

static int color_enabled(void)
{
    static int cached = -1;
    const char *term;

    if (cached != -1) {
        return cached;
    }

    if (getenv("FORCE_COLOR") != NULL) {
        cached = 1;
        return cached;
    }
    if (getenv("NO_COLOR") != NULL) {
        cached = 0;
        return cached;
    }
    if (!is_stdout_tty()) {
        cached = 0;
        return cached;
    }

    term = getenv("TERM");
    if (term != NULL && strcmp(term, "dumb") == 0) {
        cached = 0;
        return cached;
    }

#ifdef _WIN32
    {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;

        if (out == INVALID_HANDLE_VALUE || !GetConsoleMode(out, &mode)) {
            cached = 0;
            return cached;
        }
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif

    cached = 1;
    return cached;
}

static const char *clr(const char *code)
{
    return color_enabled() ? code : "";
}

static void vlog_line(const struct pak_options *opts, const char *prefix, const char *fmt, va_list ap)
{
    if (opts != NULL && opts->quiet) {
        return;
    }

    fputs(clr(CLR_CYAN), stdout);
    fputs(prefix, stdout);
    fputs(clr(CLR_RESET), stdout);
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

    printf("  %s%d/%d%s  ", clr(CLR_DIM), index, total, clr(CLR_RESET));
    fputs(clr(CLR_BOLD), stdout);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputs(clr(CLR_RESET), stdout);
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

    printf("\r      %s[", clr(CLR_DIM));
    for (i = 0; i < width; i++) {
        if (i == 0 || i == filled) {
            fputs(i < filled ? clr(CLR_GREEN) : clr(CLR_DIM), stdout);
        }
        putchar(i < filled ? '#' : '-');
    }
    printf("%s] %s%3d%%%s  %s%s%s/%s%s%s", clr(CLR_DIM), clr(percent == 100 ? CLR_GREEN : CLR_YELLOW), percent, clr(CLR_RESET), clr(CLR_BOLD), done_buf, clr(CLR_RESET), clr(CLR_DIM), total_buf, clr(CLR_RESET));

    if (force) {
        putchar('\n');
    }
    fflush(stdout);
}
