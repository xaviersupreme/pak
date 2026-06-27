#include "pak.h"

#include <stdarg.h>

static void diag_color(const char *code)
{
    fputs(pak_clr(stderr, code), stderr);
}

static void diag_label(const char *label, const char *color)
{
    fflush(stdout);
    diag_color(color);
    fputs(label, stderr);
    diag_color(PAK_CLR_RESET);
    fputc(' ', stderr);
}

static void diag_vline(const char *label, const char *color, const char *fmt, va_list ap)
{
    diag_label(label, color);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void diag_error(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    diag_vline("pak:", PAK_CLR_BOLD PAK_CLR_RED, fmt, ap);
    va_end(ap);
}

void diag_hint(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    diag_vline("hint:", PAK_CLR_YELLOW, fmt, ap);
    va_end(ap);
}

void diag_known(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    diag_vline("known:", PAK_CLR_CYAN, fmt, ap);
    va_end(ap);
}

void diag_try(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    diag_vline("try:", PAK_CLR_GREEN, fmt, ap);
    va_end(ap);
}

void diag_or(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    diag_vline("or:", PAK_CLR_DIM, fmt, ap);
    va_end(ap);
}

void diag_error_start(void)
{
    diag_label("pak:", PAK_CLR_BOLD PAK_CLR_RED);
}

void diag_hint_start(void)
{
    diag_label("hint:", PAK_CLR_YELLOW);
}

void diag_known_start(void)
{
    diag_label("known:", PAK_CLR_CYAN);
}

void diag_try_start(void)
{
    diag_label("try:", PAK_CLR_GREEN);
}

void diag_placeholder(const char *text)
{
    diag_color(PAK_CLR_DIM);
    fputs(text, stderr);
    diag_color(PAK_CLR_RESET);
}
