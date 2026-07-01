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

static char *copy_string(const char *value)
{
    char *out = malloc(strlen(value) + 1);

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
        return EACCES;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
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
            return -1;
        }
        names = malloc((size_t)capacity * sizeof(*names));
        if (names == NULL) {
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

static int collect_directory(struct path_list *list, const char *dir, size_t root_len)
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
            if (collect_directory(list, child, root_len) != 0) {
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
            if (collect_directory(list, child, root_len) != 0) {
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
        }
        free(child);
    }

    closedir(dp);
    return 0;
#endif
}

static int collect_matching_files(struct path_list *list, const char *dir, const char *pattern, int *matched)
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
            if (collect_matching_files(list, child, pattern, matched) != 0) {
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
            if (collect_matching_files(list, child, pattern, matched) != 0) {
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

    if (collect_matching_files(NULL, ".", input, &matched) != 0) {
        return 0;
    }
    return matched;
}

static int add_recursive_wildcard_input(struct path_list *list, const char *input)
{
    int matched = 0;

    if (collect_matching_files(list, ".", input, &matched) != 0) {
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
static int add_wildcard_input(struct path_list *list, const char *input, int *saw_directory)
{
    WIN32_FIND_DATAA data;
    HANDLE find;
    const char *sep;
    char *dir;
    int matched = 0;

    sep = last_path_separator(input);
    if (sep == NULL) {
        dir = copy_string(".");
    } else {
        dir = copy_path_part(input, (size_t)(sep - input));
        *saw_directory = 1;
    }
    if (dir == NULL) {
        diag_error("out of memory");
        return -1;
    }

    find = FindFirstFileA(input, &data);
    if (find == INVALID_HANDLE_VALUE) {
        diag_error("no files match '%s'", input);
        hint_recursive_matches(input);
        free(dir);
        return -1;
    }

    do {
        char *path;

        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
            continue;
        }
        matched = 1;
        path = join_scan_path(dir, data.cFileName);
        if (path == NULL) {
            diag_error("out of memory");
            FindClose(find);
            free(dir);
            return -1;
        }
        if (file_attributes_are_reparse_directory(data.dwFileAttributes)) {
            free(path);
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            *saw_directory = 1;
            if (collect_directory(list, path, scan_root_len(path)) != 0) {
                diag_error("cannot read directory '%s': %s", path, strerror(errno));
                free(path);
                FindClose(find);
                free(dir);
                return -1;
            }
        } else if (path_is_file(path)) {
            if (path_list_add(list, path, NULL) != 0) {
                diag_error("out of memory");
                free(path);
                FindClose(find);
                free(dir);
                return -1;
            }
        }
        free(path);
    } while (FindNextFileA(find, &data));

    FindClose(find);
    free(dir);
    if (!matched) {
        diag_error("no files match '%s'", input);
        hint_recursive_matches(input);
        return -1;
    }
    return 0;
}
#else
static int add_wildcard_input(struct path_list *list, const char *input, int *saw_directory)
{
    glob_t matches;
    size_t i;
    int rc;

    memset(&matches, 0, sizeof(matches));
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

        if (path_is_directory(path)) {
            *saw_directory = 1;
            if (collect_directory(list, path, scan_root_len(path)) != 0) {
                diag_error("cannot read directory '%s': %s", path, strerror(errno));
                globfree(&matches);
                return -1;
            }
        } else if (path_is_file(path)) {
            if (path_list_add(list, path, NULL) != 0) {
                diag_error("out of memory");
                globfree(&matches);
                return -1;
            }
        }
    }

    globfree(&matches);
    return 0;
}
#endif

int path_list_add_inputs(struct path_list *list, int input_count, char **inputs, int *saw_directory)
{
    int i;

    *saw_directory = 0;
    for (i = 0; i < input_count; i++) {
        if (input_has_wildcard(inputs[i])) {
            if (input_has_recursive_wildcard(inputs[i])) {
                if (add_recursive_wildcard_input(list, inputs[i]) != 0) {
                    return -1;
                }
                *saw_directory = 1;
            } else if (add_wildcard_input(list, inputs[i], saw_directory) != 0) {
                return -1;
            }
        } else if (path_is_directory(inputs[i])) {
            *saw_directory = 1;
            if (collect_directory(list, inputs[i], scan_root_len(inputs[i])) != 0) {
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

    return path_list_sort(list);
}
