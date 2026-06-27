#include "pak.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int write_temp_archive(const uint8_t *data, size_t size, char *path, size_t path_size)
{
    FILE *fp;

#ifdef _WIN32
    char dir[MAX_PATH];

    (void)path_size;
    if (GetTempPathA(sizeof(dir), dir) == 0) {
        return -1;
    }
    if (GetTempFileNameA(dir, "pak", 0, path) == 0) {
        return -1;
    }
#else
    int fd;

    snprintf(path, path_size, "/tmp/pak-fuzz-XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) {
        return -1;
    }
    close(fd);
#endif

    fp = fopen(path, "wb");
    if (fp == NULL) {
        remove(path);
        return -1;
    }
    if (fwrite(data, 1, size, fp) != size) {
        fclose(fp);
        remove(path);
        return -1;
    }
    fclose(fp);
    return 0;
}

static void suppress_stdout_once(void)
{
    static int suppressed;

    if (suppressed) {
        return;
    }
#ifdef _WIN32
    freopen("NUL", "wb", stdout);
#else
    freopen("/dev/null", "wb", stdout);
#endif
    suppressed = 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct pak_options opts;
    char path[512];
    char *missing_name = "missing-entry";

    if (size > 1024 * 1024) {
        return 0;
    }

    diag_set_suppressed(1);
    suppress_stdout_once();
    if (write_temp_archive(data, size, path, sizeof(path)) != 0) {
        return 0;
    }

    pattern_list_init(&opts.exclude_patterns);
    opts.quiet = 1;
    opts.preserve_paths = 0;
    opts.compress = 0;
    opts.store = 0;
    opts.compression_level = PAK_DEFAULT_COMPRESSION_LEVEL;
    opts.long_list = 0;
    opts.full_names = 0;
    opts.overwrite_mode = PAK_OVERWRITE_REFUSE;
    opts.use_pakignore = 0;
    opts.extract_dir = NULL;
    opts.option_mask = 0;
    opts.seen_option_count = 0;

    pak_check(path, &opts);
    pak_list(path, 0, NULL, &opts);
    pak_info(path, &opts);
    pak_cat(path, missing_name, &opts);
    pak_delete(path, 1, &missing_name, &opts);
    pak_rename(path, missing_name, "renamed-entry", &opts);
    pak_repack(path, 1, &missing_name, &opts);
    pak_extract(path, 1, &missing_name, &opts);

    pattern_list_free(&opts.exclude_patterns);
    remove(path);
    return 0;
}

#ifdef PAK_FUZZ_STANDALONE
static uint32_t fuzz_rand(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

int main(int argc, char **argv)
{
    uint8_t data[8192];
    unsigned long iterations = 10000;
    uint32_t state = 0x70616b31u;
    unsigned long i;

    if (argc > 1) {
        iterations = strtoul(argv[1], NULL, 10);
        if (iterations == 0) {
            iterations = 1;
        }
    }

    for (i = 0; i < iterations; i++) {
        size_t size = (size_t)(fuzz_rand(&state) % sizeof(data));
        size_t j;

        for (j = 0; j < size; j++) {
            data[j] = (uint8_t)(fuzz_rand(&state) >> 24);
        }

        if (size >= 8 && (i % 4) == 0) {
            memcpy(data, (i % 8) == 0 ? "PAK1" : "PAK2", 4);
            data[4] = (uint8_t)(fuzz_rand(&state) % 8);
            data[5] = 0;
            data[6] = 0;
            data[7] = 0;
        }

        LLVMFuzzerTestOneInput(data, size);
    }

    fprintf(stderr, "fuzz smoke: %lu inputs\n", iterations);
    return 0;
}
#endif
