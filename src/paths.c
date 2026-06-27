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

static int path_is_directory(const char *path)
{
#ifdef _WIN32
    struct _stat64 st;

    return _stat64(path, &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
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

void path_list_init(struct path_list *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void path_list_free(struct path_list *list)
{
    int i;

    for (i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    path_list_init(list);
}

static int path_list_add(struct path_list *list, const char *path)
{
    char **items;
    int capacity;

    if (list->count == list->capacity) {
        capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        items = realloc(list->items, (size_t)capacity * sizeof(*items));
        if (items == NULL) {
            return -1;
        }
        list->items = items;
        list->capacity = capacity;
    }

    list->items[list->count] = copy_string(path);
    if (list->items[list->count] == NULL) {
        return -1;
    }
    list->count++;
    return 0;
}

static int compare_paths(const void *left, const void *right)
{
    const char *a = *(const char * const *)left;
    const char *b = *(const char * const *)right;

    return strcmp(a, b);
}

static int collect_directory(struct path_list *list, const char *dir)
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
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (collect_directory(list, child) != 0) {
                free(child);
                FindClose(find);
                return -1;
            }
        } else if (path_is_file(child)) {
            if (path_list_add(list, child) != 0) {
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
            if (collect_directory(list, child) != 0) {
                free(child);
                closedir(dp);
                return -1;
            }
        } else if (path_is_file(child)) {
            if (path_list_add(list, child) != 0) {
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

int path_list_add_inputs(struct path_list *list, int input_count, char **inputs, int *saw_directory)
{
    int i;

    *saw_directory = 0;
    for (i = 0; i < input_count; i++) {
        if (path_is_directory(inputs[i])) {
            *saw_directory = 1;
            if (collect_directory(list, inputs[i]) != 0) {
                diag_error("cannot read directory '%s': %s", inputs[i], strerror(errno));
                return -1;
            }
        } else if (path_is_file(inputs[i])) {
            if (path_list_add(list, inputs[i]) != 0) {
                diag_error("out of memory");
                return -1;
            }
        } else {
            diag_error("cannot pack '%s': not a regular file or directory", inputs[i]);
            return -1;
        }
    }

    qsort(list->items, (size_t)list->count, sizeof(*list->items), compare_paths);
    return 0;
}
