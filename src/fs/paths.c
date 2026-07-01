#include "pak.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <glob.h>
#include <sys/stat.h>
#endif

#define RECURSIVE_HINT_VISIT_LIMIT 256
#define PATH_SCAN_REPORT_STEP 16384ull

struct path_scan_stats {
    const struct pak_options *opts;
    uint64_t dirs;
    uint64_t files;
    uint64_t skipped_dirs;
    uint64_t next_report;
    int active;
};

static int collect_matching_files_limited(struct path_list *list, const char *dir, const char *pattern, int *matched, int *visited, int visit_limit, struct path_scan_stats *stats);

static char *copy_string(const char *value)
{
    char *out = malloc(strlen(value) + 1);

    if (out == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    if (out != NULL) {
        strcpy(out, value);
    }
    return out;
}

static char *join_scan_path(const char *dir, const char *name)
{
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    int need_sep = dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';
    char *out = malloc(dir_len + (size_t)need_sep + name_len + 1);

    if (out == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(out, dir, dir_len);
    if (need_sep) {
        out[dir_len++] = '/';
    }
    memcpy(out + dir_len, name, name_len + 1);
    return out;
}

static size_t scan_root_len(const char *path)
{
    size_t len = strlen(path);

    while (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\')) {
        len--;
    }
    return len;
}

static const char *relative_scan_name(const char *path, size_t root_len)
{
    const char *name = path + root_len;

    while (*name == '/' || *name == '\\') {
        name++;
    }
    return name;
}

#ifdef _WIN32
static int errno_from_win32_error(DWORD error)
{
    switch (error) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return EACCES;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE:
        return ENAMETOOLONG;
    case ERROR_INVALID_NAME:
        return EINVAL;
    default:
        return EINVAL;
    }
}

static int file_attributes_are_reparse_directory(DWORD attrs)
{
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

static char *copy_path_part(const char *path, size_t len)
{
    char *out = malloc(len + 1);

    if (out == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}
#endif

static const char *last_path_separator(const char *path)
{
    const char *last = NULL;

    while (*path != '\0') {
        if (*path == '/' || *path == '\\') {
            last = path;
        }
        path++;
    }
    return last;
}

static size_t wildcard_parent_root_len(const char *input)
{
    const char *p;
    const char *sep = NULL;
    size_t len;

    for (p = input; *p != '\0'; p++) {
        if (*p == '*' || *p == '?') {
            break;
        }
        if (*p == '/' || *p == '\\') {
            sep = p;
        }
    }
    if (sep == NULL) {
        return 0;
    }
    if (sep == input) {
        return 1;
    }
    len = (size_t)(sep - input);
    while (len > 0 && (input[len - 1] == '/' || input[len - 1] == '\\')) {
        len--;
    }
    return len;
}

#ifndef _WIN32
static int path_is_dot_entry(const char *path)
{
    const char *name = io_base_name(path);

    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}
#endif

static int input_has_wildcard(const char *input)
{
    while (*input != '\0') {
        if (*input == '*' || *input == '?') {
            return 1;
        }
        input++;
    }
    return 0;
}

static int input_has_recursive_wildcard(const char *input)
{
    while (input[0] != '\0' && input[1] != '\0') {
        if (input[0] == '*' && input[1] == '*') {
            return 1;
        }
        input++;
    }
    return 0;
}

static int path_is_directory(const char *path)
{
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);

    return attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int path_is_file(const char *path)
{
    uint64_t size;

    return io_file_size(path, &size) == 0;
}

static int directory_scan_error_is_skippable(int err)
{
    return err == EACCES || err == ENOENT || err == ENOTDIR
#ifdef ENAMETOOLONG
        || err == ENAMETOOLONG
#endif
#ifdef _WIN32
        || err == EINVAL
#endif
        ;
}

static void path_scan_init(struct path_scan_stats *stats, const struct pak_options *opts)
{
    memset(stats, 0, sizeof(*stats));
    stats->opts = opts;
    stats->next_report = PATH_SCAN_REPORT_STEP;
}

static uint64_t path_scan_total(const struct path_scan_stats *stats)
{
    return stats->dirs + stats->files + stats->skipped_dirs;
}

static void path_scan_begin(struct path_scan_stats *stats, const char *path)
{
    if (stats == NULL) {
        return;
    }
    stats->active = 1;
    log_step(stats->opts, "scan %s", path);
}

static void path_scan_report(struct path_scan_stats *stats, const char *path)
{
    uint64_t total;

    if (stats == NULL || !stats->active) {
        return;
    }

    total = path_scan_total(stats);
    if (total < stats->next_report) {
        return;
    }

    log_step(
        stats->opts,
        "scanned %llu paths, found %llu files, skipped %llu dirs (now %s)",
        (unsigned long long)total,
        (unsigned long long)stats->files,
        (unsigned long long)stats->skipped_dirs,
        path);
    while (stats->next_report <= total) {
        stats->next_report += PATH_SCAN_REPORT_STEP;
    }
}

static void path_scan_note_dir(struct path_scan_stats *stats, const char *path)
{
    if (stats == NULL) {
        return;
    }
    stats->dirs++;
    path_scan_report(stats, path);
}

static void path_scan_note_file(struct path_scan_stats *stats, const char *path)
{
    if (stats == NULL) {
        return;
    }
    stats->files++;
    path_scan_report(stats, path);
}

static void path_scan_skip_dir(struct path_scan_stats *stats, const char *path, int err)
{
    if (stats == NULL) {
        return;
    }
    stats->skipped_dirs++;
    log_step(stats->opts, "skip directory %s: %s", path, strerror(err));
    path_scan_report(stats, path);
}

static void path_scan_finish(struct path_scan_stats *stats)
{
    if (stats == NULL || !stats->active) {
        return;
    }

    log_step(
        stats->opts,
        "scan found %llu file%s, skipped %llu director%s",
        (unsigned long long)stats->files,
        stats->files == 1 ? "" : "s",
        (unsigned long long)stats->skipped_dirs,
        stats->skipped_dirs == 1 ? "y" : "ies");
}

static int path_exists_for_input(const char *path)
{
#ifdef _WIN32
    struct _stat64 st;

    return _stat64(path, &st) == 0;
#else
    struct stat st;

    return stat(path, &st) == 0;
#endif
}

void path_list_init(struct path_list *list)
{
    list->items = NULL;
    list->names = NULL;
    list->count = 0;
    list->capacity = 0;
}

void path_list_free(struct path_list *list)
{
    int i;

    for (i = 0; i < list->count; i++) {
        free(list->items[i]);
        free(list->names[i]);
    }
    free(list->items);
    free(list->names);
    path_list_init(list);
}

static int path_list_add(struct path_list *list, const char *path, const char *name)
{
    char **items;
    char **names;
    int capacity;

    if (list->count == list->capacity) {
        capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        items = malloc((size_t)capacity * sizeof(*items));
        if (items == NULL) {
            errno = ENOMEM;
            return -1;
        }
        names = malloc((size_t)capacity * sizeof(*names));
        if (names == NULL) {
            errno = ENOMEM;
            free(items);
            return -1;
        }
        if (list->count > 0) {
            memcpy(items, list->items, (size_t)list->count * sizeof(*items));
            memcpy(names, list->names, (size_t)list->count * sizeof(*names));
        }
        free(list->items);
        free(list->names);
        list->items = items;
        list->names = names;
        list->capacity = capacity;
    }

    list->items[list->count] = copy_string(path);
    if (list->items[list->count] == NULL) {
        return -1;
    }
    list->names[list->count] = name == NULL ? NULL : copy_string(name);
    if (name != NULL && list->names[list->count] == NULL) {
        free(list->items[list->count]);
        return -1;
    }
    list->count++;
    return 0;
}

struct path_pair {
    char *path;
    char *name;
};

static int compare_path_pairs(const void *left, const void *right)
{
    const struct path_pair *a = left;
    const struct path_pair *b = right;

    return strcmp(a->name != NULL ? a->name : a->path, b->name != NULL ? b->name : b->path);
}

static int path_list_sort(struct path_list *list)
{
    struct path_pair *pairs;
    int i;

    if (list->count <= 1) {
        return 0;
    }
    pairs = malloc((size_t)list->count * sizeof(*pairs));
    if (pairs == NULL) {
        return -1;
    }
    for (i = 0; i < list->count; i++) {
        pairs[i].path = list->items[i];
        pairs[i].name = list->names[i];
    }
    qsort(pairs, (size_t)list->count, sizeof(*pairs), compare_path_pairs);
    for (i = 0; i < list->count; i++) {
        list->items[i] = pairs[i].path;
        list->names[i] = pairs[i].name;
    }
    free(pairs);
    return 0;
}

static int collect_directory(struct path_list *list, const char *dir, size_t root_len, struct path_scan_stats *stats)
{
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    HANDLE find;
    char *pattern = join_scan_path(dir, "*");

    if (pattern == NULL) {
        return -1;
    }
    find = FindFirstFileA(pattern, &data);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        errno = errno_from_win32_error(GetLastError());
        return -1;
    }

    do {
        char *child;

        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
            continue;
        }
        child = join_scan_path(dir, data.cFileName);
        if (child == NULL) {
            FindClose(find);
            return -1;
        }
        if (file_attributes_are_reparse_directory(data.dwFileAttributes)) {
            free(child);
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            path_scan_note_dir(stats, child);
            if (collect_directory(list, child, root_len, stats) != 0) {
                if (directory_scan_error_is_skippable(errno)) {
                    path_scan_skip_dir(stats, child, errno);
                    free(child);
                    continue;
                }
                free(child);
                FindClose(find);
                return -1;
            }
        } else if (path_is_file(child)) {
            if (path_list_add(list, child, relative_scan_name(child, root_len)) != 0) {
                free(child);
                FindClose(find);
                return -1;
            }
            path_scan_note_file(stats, child);
        }
        free(child);
    } while (FindNextFileA(find, &data));

    FindClose(find);
    return 0;
#else
    DIR *dp;
    struct dirent *entry;

    dp = opendir(dir);
    if (dp == NULL) {
        return -1;
    }

    while ((entry = readdir(dp)) != NULL) {
        char *child;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        child = join_scan_path(dir, entry->d_name);
        if (child == NULL) {
            closedir(dp);
            return -1;
        }
        if (path_is_directory(child)) {
            path_scan_note_dir(stats, child);
            if (collect_directory(list, child, root_len, stats) != 0) {
                if (directory_scan_error_is_skippable(errno)) {
                    path_scan_skip_dir(stats, child, errno);
                    free(child);
                    continue;
                }
                free(child);
                closedir(dp);
                return -1;
            }
        } else if (path_is_file(child)) {
            if (path_list_add(list, child, relative_scan_name(child, root_len)) != 0) {
                free(child);
                closedir(dp);
                return -1;
            }
            path_scan_note_file(stats, child);
        }
        free(child);
    }

    closedir(dp);
    return 0;
#endif
}

static int collect_matching_files(struct path_list *list, const char *dir, const char *pattern, int *matched, struct path_scan_stats *stats)
{
    return collect_matching_files_limited(list, dir, pattern, matched, NULL, 0, stats);
}

static int collect_matching_files_limited(struct path_list *list, const char *dir, const char *pattern, int *matched, int *visited, int visit_limit, struct path_scan_stats *stats)
{
#ifdef _WIN32
    WIN32_FIND_DATAA data;
    HANDLE find;
    char *scan = join_scan_path(dir, "*");

    if (scan == NULL) {
        return -1;
    }
    find = FindFirstFileA(scan, &data);
    free(scan);
    if (find == INVALID_HANDLE_VALUE) {
        errno = errno_from_win32_error(GetLastError());
        return -1;
    }

    do {
        char *child;

        if (visit_limit > 0 && visited != NULL) {
            if (*visited >= visit_limit) {
                break;
            }
            *visited += 1;
        }
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
            continue;
        }
        child = join_scan_path(dir, data.cFileName);
        if (child == NULL) {
            FindClose(find);
            return -1;
        }
        if (file_attributes_are_reparse_directory(data.dwFileAttributes)) {
            free(child);
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            path_scan_note_dir(stats, child);
            if (collect_matching_files_limited(list, child, pattern, matched, visited, visit_limit, stats) != 0) {
                if (directory_scan_error_is_skippable(errno)) {
                    path_scan_skip_dir(stats, child, errno);
                    free(child);
                    continue;
                }
                free(child);
                FindClose(find);
                return -1;
            }
        } else if (path_is_file(child) && pak_pattern_match(pattern, child)) {
            if (list != NULL && path_list_add(list, child, NULL) != 0) {
                free(child);
                FindClose(find);
                return -1;
            }
            *matched += 1;
            path_scan_note_file(stats, child);
        }
        free(child);
    } while (FindNextFileA(find, &data));

    FindClose(find);
    return 0;
#else
    DIR *dp;
    struct dirent *entry;

    dp = opendir(dir);
    if (dp == NULL) {
        return -1;
    }

    while ((entry = readdir(dp)) != NULL) {
        char *child;

        if (visit_limit > 0 && visited != NULL) {
            if (*visited >= visit_limit) {
                break;
            }
            *visited += 1;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        child = join_scan_path(dir, entry->d_name);
        if (child == NULL) {
            closedir(dp);
            return -1;
        }
        if (path_is_directory(child)) {
            path_scan_note_dir(stats, child);
            if (collect_matching_files_limited(list, child, pattern, matched, visited, visit_limit, stats) != 0) {
                if (directory_scan_error_is_skippable(errno)) {
                    path_scan_skip_dir(stats, child, errno);
                    free(child);
                    continue;
                }
                free(child);
                closedir(dp);
                return -1;
            }
        } else if (path_is_file(child) && pak_pattern_match(pattern, child)) {
            if (list != NULL && path_list_add(list, child, NULL) != 0) {
                free(child);
                closedir(dp);
                return -1;
            }
            *matched += 1;
            path_scan_note_file(stats, child);
        }
        free(child);
    }

    closedir(dp);
    return 0;
#endif
}

static int count_recursive_matches(const char *input)
{
    int matched = 0;
    int visited = 0;

    if (collect_matching_files_limited(NULL, ".", input, &matched, &visited, RECURSIVE_HINT_VISIT_LIMIT, NULL) != 0) {
        return 0;
    }
    return matched;
}

static int add_recursive_wildcard_input(struct path_list *list, const char *input, struct path_scan_stats *stats)
{
    int matched = 0;

    path_scan_begin(stats, input);
    path_scan_note_dir(stats, ".");
    if (collect_matching_files(list, ".", input, &matched, stats) != 0) {
        diag_error("cannot expand '%s'", input);
        return -1;
    }
    if (matched == 0) {
        diag_error("no files match '%s'", input);
        return -1;
    }
    return 0;
}

static void hint_recursive_matches(const char *input)
{
    int matches;

    if (input_has_recursive_wildcard(input)) {
        return;
    }
    matches = count_recursive_matches(input);
    if (matches <= 0) {
        return;
    }

    if (last_path_separator(input) == NULL) {
        diag_hint("found %d match%s in subdirectories; try \"**/%s\"", matches, matches == 1 ? "" : "es", input);
    } else {
        diag_hint("found %d match%s in subdirectories; use ** for recursive matches", matches, matches == 1 ? "" : "es");
    }
}

#ifdef _WIN32
static int path_separator_char(char ch)
{
    return ch == '/' || ch == '\\';
}

static const char *skip_path_separators(const char *path)
{
    while (path_separator_char(*path)) {
        path++;
    }
    return path;
}

static const char *windows_wildcard_start_dir(const char *input, char **out_dir)
{
    const char *p;
    const char *server;
    const char *share;

    if (((input[0] >= 'A' && input[0] <= 'Z') || (input[0] >= 'a' && input[0] <= 'z')) && input[1] == ':') {
        if (path_separator_char(input[2])) {
            *out_dir = copy_path_part(input, 3);
            return skip_path_separators(input + 3);
        }
        *out_dir = copy_path_part(input, 2);
        return skip_path_separators(input + 2);
    }

    if (path_separator_char(input[0]) && path_separator_char(input[1])) {
        server = input + 2;
        p = server;
        while (*p != '\0' && !path_separator_char(*p)) {
            p++;
        }
        if (*p != '\0') {
            share = skip_path_separators(p);
            p = share;
            while (*p != '\0' && !path_separator_char(*p)) {
                p++;
            }
            *out_dir = copy_path_part(input, (size_t)(p - input));
            return skip_path_separators(p);
        }
    }

    if (path_separator_char(input[0])) {
        *out_dir = copy_path_part(input, 1);
        return skip_path_separators(input + 1);
    }

    *out_dir = copy_string("");
    return input;
}

static const char *next_windows_wildcard_segment(const char *input, char **out_segment, int *is_last)
{
    const char *start;
    const char *end;
    const char *next;

    start = skip_path_separators(input);
    if (*start == '\0') {
        *out_segment = NULL;
        *is_last = 1;
        return start;
    }

    end = start;
    while (*end != '\0' && !path_separator_char(*end)) {
        end++;
    }
    next = skip_path_separators(end);
    *out_segment = copy_path_part(start, (size_t)(end - start));
    *is_last = *next == '\0';
    return next;
}

static int add_windows_wildcard_path(struct path_list *list, const char *path, DWORD attrs, size_t root_len, int is_last, const char *next, int *matched, int *saw_directory, struct path_scan_stats *stats);

static int collect_windows_wildcard_segments(struct path_list *list, const char *dir, const char *input, size_t root_len, int *matched, int *saw_directory, struct path_scan_stats *stats)
{
    WIN32_FIND_DATAA data;
    DWORD attrs;
    HANDLE find;
    char *segment;
    char *scan;
    char *path;
    const char *next;
    int is_last;

    next = next_windows_wildcard_segment(input, &segment, &is_last);
    if (segment == NULL) {
        return 0;
    }

    if (!input_has_wildcard(segment)) {
        path = join_scan_path(dir, segment);
        free(segment);
        if (path == NULL) {
            return -1;
        }
        attrs = GetFileAttributesA(path);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            free(path);
            return 0;
        }
        if (add_windows_wildcard_path(list, path, attrs, root_len, is_last, next, matched, saw_directory, stats) != 0) {
            free(path);
            return -1;
        }
        free(path);
        return 0;
    }

    scan = join_scan_path(dir, segment);
    free(segment);
    if (scan == NULL) {
        return -1;
    }
    find = FindFirstFileA(scan, &data);
    free(scan);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }

    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
            continue;
        }
        path = join_scan_path(dir, data.cFileName);
        if (path == NULL) {
            FindClose(find);
            return -1;
        }
        if (add_windows_wildcard_path(list, path, data.dwFileAttributes, root_len, is_last, next, matched, saw_directory, stats) != 0) {
            free(path);
            FindClose(find);
            return -1;
        }
        free(path);
    } while (FindNextFileA(find, &data));

    FindClose(find);
    return 0;
}

static int add_windows_wildcard_path(struct path_list *list, const char *path, DWORD attrs, size_t root_len, int is_last, const char *next, int *matched, int *saw_directory, struct path_scan_stats *stats)
{
    if (file_attributes_are_reparse_directory(attrs)) {
        return 0;
    }
    if (!is_last) {
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return 0;
        }
        path_scan_note_dir(stats, path);
        return collect_windows_wildcard_segments(list, path, next, root_len, matched, saw_directory, stats);
    }

    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        *matched = 1;
        *saw_directory = 1;
        path_scan_note_dir(stats, path);
        if (collect_directory(list, path, root_len, stats) != 0) {
            diag_error("cannot read directory '%s': %s", path, strerror(errno));
            return -1;
        }
    } else if (path_is_file(path)) {
        *matched = 1;
        if (path_list_add(list, path, NULL) != 0) {
            diag_error("out of memory");
            return -1;
        }
        path_scan_note_file(stats, path);
    }
    return 0;
}

static int add_wildcard_input(struct path_list *list, const char *input, int *saw_directory, struct path_scan_stats *stats)
{
    char *dir;
    const char *rest;
    int matched = 0;

    rest = windows_wildcard_start_dir(input, &dir);
    if (dir == NULL) {
        diag_error("out of memory");
        return -1;
    }

    path_scan_begin(stats, input);
    if (collect_windows_wildcard_segments(list, dir, rest, wildcard_parent_root_len(input), &matched, saw_directory, stats) != 0) {
        free(dir);
        return -1;
    }
    free(dir);
    if (!matched) {
        diag_error("no files match '%s'", input);
        hint_recursive_matches(input);
        return -1;
    }
    return 0;
}
#else
static int add_wildcard_input(struct path_list *list, const char *input, int *saw_directory, struct path_scan_stats *stats)
{
    glob_t matches;
    size_t i;
    size_t dir_root_len;
    int matched = 0;
    int rc;

    memset(&matches, 0, sizeof(matches));
    dir_root_len = wildcard_parent_root_len(input);
    path_scan_begin(stats, input);
    rc = glob(input, GLOB_NOSORT, NULL, &matches);
    if (rc == GLOB_NOMATCH) {
        diag_error("no files match '%s'", input);
        hint_recursive_matches(input);
        globfree(&matches);
        return -1;
    }
    if (rc != 0) {
        diag_error("cannot expand '%s'", input);
        globfree(&matches);
        return -1;
    }

    for (i = 0; i < matches.gl_pathc; i++) {
        const char *path = matches.gl_pathv[i];

        if (path_is_dot_entry(path)) {
            continue;
        }
        if (path_is_directory(path)) {
            *saw_directory = 1;
            matched = 1;
            path_scan_note_dir(stats, path);
            if (collect_directory(list, path, dir_root_len, stats) != 0) {
                diag_error("cannot read directory '%s': %s", path, strerror(errno));
                globfree(&matches);
                return -1;
            }
        } else if (path_is_file(path)) {
            matched = 1;
            if (path_list_add(list, path, NULL) != 0) {
                diag_error("out of memory");
                globfree(&matches);
                return -1;
            }
            path_scan_note_file(stats, path);
        }
    }

    globfree(&matches);
    if (!matched) {
        diag_error("no files match '%s'", input);
        hint_recursive_matches(input);
        return -1;
    }
    return 0;
}
#endif

int path_list_add_inputs(struct path_list *list, int input_count, char **inputs, int *saw_directory, const struct pak_options *opts)
{
    struct path_scan_stats stats;
    int i;

    path_scan_init(&stats, opts);
    *saw_directory = 0;
    for (i = 0; i < input_count; i++) {
        if (input_has_wildcard(inputs[i])) {
            if (input_has_recursive_wildcard(inputs[i])) {
                if (add_recursive_wildcard_input(list, inputs[i], &stats) != 0) {
                    return -1;
                }
                *saw_directory = 1;
            } else if (add_wildcard_input(list, inputs[i], saw_directory, &stats) != 0) {
                return -1;
            }
        } else if (path_is_directory(inputs[i])) {
            *saw_directory = 1;
            path_scan_begin(&stats, inputs[i]);
            path_scan_note_dir(&stats, inputs[i]);
            if (collect_directory(list, inputs[i], scan_root_len(inputs[i]), &stats) != 0) {
                diag_error("cannot read directory '%s': %s", inputs[i], strerror(errno));
                return -1;
            }
        } else if (path_is_file(inputs[i])) {
            if (path_list_add(list, inputs[i], NULL) != 0) {
                diag_error("out of memory");
                return -1;
            }
        } else if (!path_exists_for_input(inputs[i])) {
            diag_error("cannot pack '%s': not found", inputs[i]);
            return -1;
        } else {
            diag_error("cannot pack '%s': not a regular file or directory", inputs[i]);
            return -1;
        }
    }

    path_scan_finish(&stats);
    return path_list_sort(list);
}
