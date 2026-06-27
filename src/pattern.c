#include "pak.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *value)
{
    char *out = malloc(strlen(value) + 1);

    if (out != NULL) {
        strcpy(out, value);
    }
    return out;
}

static int is_space_char(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static char *trim_line(char *line)
{
    char *end;

    while (is_space_char(*line)) {
        line++;
    }

    end = line + strlen(line);
    while (end > line && is_space_char(end[-1])) {
        end--;
    }
    *end = '\0';

    return line;
}

static void strip_leading_current_dir(char *path)
{
    while (path[0] == '.' && path[1] == '/') {
        memmove(path, path + 2, strlen(path + 2) + 1);
    }
}

static char *normalize_pattern(const char *pattern)
{
    char *out;
    char *w;
    const char *r;

    out = copy_string(pattern);
    if (out == NULL) {
        return NULL;
    }

    for (w = out, r = out; *r != '\0'; r++) {
        char ch = *r == '\\' ? '/' : *r;

        if (ch == '/' && w == out) {
            continue;
        }
        if (ch == '/' && w > out && w[-1] == '/') {
            continue;
        }
        *w++ = ch;
    }
    *w = '\0';

    strip_leading_current_dir(out);
    return out;
}

static int has_slash(const char *value)
{
    while (*value != '\0') {
        if (*value == '/') {
            return 1;
        }
        value++;
    }
    return 0;
}

static const char *path_base_name(const char *path)
{
    const char *base = path;

    while (*path != '\0') {
        if (*path == '/') {
            base = path + 1;
        }
        path++;
    }
    return base;
}

static int wildcard_match(const char *pattern, const char *text)
{
    while (*pattern != '\0') {
        if (*pattern == '*') {
            while (pattern[1] == '*') {
                pattern++;
            }
            pattern++;
            if (*pattern == '\0') {
                return 1;
            }
            while (*text != '\0') {
                if (wildcard_match(pattern, text)) {
                    return 1;
                }
                text++;
            }
            return wildcard_match(pattern, text);
        }
        if (*pattern == '?') {
            if (*text == '\0') {
                return 0;
            }
            pattern++;
            text++;
            continue;
        }
        if (*pattern != *text) {
            return 0;
        }
        pattern++;
        text++;
    }

    return *text == '\0';
}

static int wildcard_match_part(const char *pattern, const char *part, size_t part_len)
{
    char *tmp;
    int matched;

    tmp = malloc(part_len + 1);
    if (tmp == NULL) {
        return 0;
    }
    memcpy(tmp, part, part_len);
    tmp[part_len] = '\0';

    matched = wildcard_match(pattern, tmp);
    free(tmp);
    return matched;
}

static int component_dir_match(const char *pattern, const char *path)
{
    const char *part = path;
    const char *end;

    while (*part != '\0') {
        end = part;
        while (*end != '\0' && *end != '/') {
            end++;
        }
        if (*end != '/') {
            return 0;
        }
        if (wildcard_match_part(pattern, part, (size_t)(end - part))) {
            return 1;
        }
        part = end + 1;
    }

    return 0;
}

static int dir_pattern_match(const char *pattern, const char *path)
{
    char *dir;
    size_t len;
    int matched;

    len = strlen(pattern);
    while (len > 0 && pattern[len - 1] == '/') {
        len--;
    }
    if (len == 0) {
        return 0;
    }

    dir = malloc(len + 1);
    if (dir == NULL) {
        return 0;
    }
    memcpy(dir, pattern, len);
    dir[len] = '\0';

    if (has_slash(dir)) {
        matched = strncmp(path, dir, len) == 0 && path[len] == '/';
    } else {
        matched = component_dir_match(dir, path);
    }

    free(dir);
    return matched;
}

void pattern_list_init(struct pak_pattern_list *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void pattern_list_free(struct pak_pattern_list *list)
{
    int i;

    for (i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    pattern_list_init(list);
}

int pattern_list_add(struct pak_pattern_list *list, const char *pattern)
{
    char **items;
    char *normalized;
    int capacity;

    normalized = normalize_pattern(pattern);
    if (normalized == NULL) {
        return -1;
    }
    if (normalized[0] == '\0') {
        free(normalized);
        return 0;
    }

    if (list->count == list->capacity) {
        capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        items = realloc(list->items, (size_t)capacity * sizeof(*items));
        if (items == NULL) {
            free(normalized);
            return -1;
        }
        list->items = items;
        list->capacity = capacity;
    }

    list->items[list->count++] = normalized;
    return 0;
}

int pattern_list_load_file(struct pak_pattern_list *list, const char *path)
{
    FILE *fp;
    char line[4096];

    fp = fopen(path, "r");
    if (fp == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *trimmed = trim_line(line);

        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }
        if (pattern_list_add(list, trimmed) != 0) {
            fclose(fp);
            errno = ENOMEM;
            return -1;
        }
    }

    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

int pak_pattern_has_magic(const char *pattern)
{
    while (*pattern != '\0') {
        if (*pattern == '*' || *pattern == '?') {
            return 1;
        }
        pattern++;
    }
    return 0;
}

int pak_pattern_match(const char *pattern, const char *path)
{
    char *normalized_pattern;
    char *normalized_path;
    int matched;
    size_t len;

    normalized_pattern = normalize_pattern(pattern);
    if (normalized_pattern == NULL) {
        return 0;
    }
    normalized_path = normalize_pattern(path);
    if (normalized_path == NULL) {
        free(normalized_pattern);
        return 0;
    }

    len = strlen(normalized_pattern);
    if (len > 0 && normalized_pattern[len - 1] == '/') {
        matched = dir_pattern_match(normalized_pattern, normalized_path);
    } else if (has_slash(normalized_pattern)) {
        matched = wildcard_match(normalized_pattern, normalized_path);
    } else {
        matched = wildcard_match(normalized_pattern, path_base_name(normalized_path));
    }

    free(normalized_path);
    free(normalized_pattern);
    return matched;
}

int pak_is_excluded(const struct pak_options *opts, const char *archive_name)
{
    int i;

    if (opts == NULL) {
        return 0;
    }

    for (i = 0; i < opts->exclude_patterns.count; i++) {
        if (pak_pattern_match(opts->exclude_patterns.items[i], archive_name)) {
            return 1;
        }
    }

    return 0;
}
