#include "pak.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#define mkdir_one(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define mkdir_one(path) mkdir(path, 0777)
#endif

int io_file_size(const char *path, uint64_t *size)
{
#ifdef _WIN32
    struct _stat64 st;

    if (_stat64(path, &st) != 0) {
        return -1;
    }
    if ((st.st_mode & _S_IFREG) == 0) {
        errno = EINVAL;
        return -1;
    }
    *size = (uint64_t)st.st_size;
#else
    struct stat st;

    if (stat(path, &st) != 0) {
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    *size = (uint64_t)st.st_size;
#endif

    return 0;
}

int io_file_exists(const char *path)
{
#ifdef _WIN32
    struct _stat64 st;
    return _stat64(path, &st) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

const char *io_base_name(const char *path)
{
    const char *name;
    const char *p;

    name = path;
    for (p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }

    return name;
}

int io_is_plain_name(const char *name)
{
    const char *p;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }

    for (p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            return 0;
        }
    }

    return 1;
}

int io_is_safe_path(const char *name)
{
    const char *p;
    const char *part;
    size_t len;

    if (name == NULL || name[0] == '\0' || name[0] == '/' || name[0] == '\\') {
        return 0;
    }
    if (name[1] == ':') {
        return 0;
    }

    part = name;
    for (p = name; ; p++) {
        if (*p == '\\' || *p == ':') {
            return 0;
        }
        if (*p == '/' || *p == '\0') {
            len = (size_t)(p - part);
            if (len == 0) {
                return 0;
            }
            if ((len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.')) {
                return 0;
            }
            if (*p == '\0') {
                break;
            }
            part = p + 1;
        }
    }

    return 1;
}

static void strip_leading_current_dir(char *path)
{
    while (path[0] == '.' && path[1] == '/') {
        memmove(path, path + 2, strlen(path + 2) + 1);
    }
}

char *io_archive_name(const char *path, int preserve_paths)
{
    const char *src;
    char *out;
    char *w;

    src = preserve_paths ? path : io_base_name(path);
    out = malloc(strlen(src) + 1);
    if (out == NULL) {
        return NULL;
    }

    for (w = out; *src != '\0'; src++, w++) {
        *w = *src == '\\' ? '/' : *src;
    }
    *w = '\0';
    if (preserve_paths) {
        strip_leading_current_dir(out);
    }

    if ((preserve_paths && !io_is_safe_path(out)) || (!preserve_paths && !io_is_plain_name(out))) {
        free(out);
        errno = EINVAL;
        return NULL;
    }

    return out;
}

char *io_join_path(const char *dir, const char *name)
{
    size_t dir_len;
    size_t name_len;
    int need_sep;
    char *out;

    if (dir == NULL || dir[0] == '\0' || strcmp(dir, ".") == 0) {
        out = malloc(strlen(name) + 1);
        if (out != NULL) {
            strcpy(out, name);
        }
        return out;
    }

    dir_len = strlen(dir);
    name_len = strlen(name);
    need_sep = dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';
    out = malloc(dir_len + (size_t)need_sep + name_len + 1);
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

int io_make_parent_dirs(const char *path)
{
    char *tmp;
    char *p;

    tmp = malloc(strlen(path) + 1);
    if (tmp == NULL) {
        return -1;
    }
    strcpy(tmp, path);

    for (p = tmp; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            char old = *p;
            *p = '\0';
            if (tmp[0] != '\0' && mkdir_one(tmp) != 0 && errno != EEXIST) {
                free(tmp);
                return -1;
            }
            *p = old;
        }
    }

    free(tmp);
    return 0;
}
