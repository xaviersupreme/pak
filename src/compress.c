#include "pak.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RLE_MARK 0xffu
#define COPY_BUF_SIZE 65536u

int rle_compress(const unsigned char *in, size_t in_size, unsigned char **out, size_t *out_size)
{
    unsigned char *buf;
    size_t cap;
    size_t i;
    size_t w;

    if (in_size > ((size_t)-1 - 3) / 3) {
        return -1;
    }

    cap = in_size * 3 + 3;
    buf = malloc(cap == 0 ? 1 : cap);
    if (buf == NULL) {
        return -1;
    }

    i = 0;
    w = 0;
    while (i < in_size) {
        size_t run = 1;

        while (i + run < in_size && in[i + run] == in[i] && run < 255) {
            run++;
        }

        if (run >= 4 || in[i] == RLE_MARK) {
            buf[w++] = RLE_MARK;
            buf[w++] = (unsigned char)run;
            buf[w++] = in[i];
        } else {
            size_t j;
            for (j = 0; j < run; j++) {
                buf[w++] = in[i + j];
            }
        }

        i += run;
    }

    *out = buf;
    *out_size = w;
    return 0;
}

int rle_decompress(FILE *in, FILE *out, uint64_t in_size, uint64_t out_size, uint32_t *crc, const char *name, const struct pak_options *opts)
{
    uint64_t read_total;
    uint64_t wrote_total;

    read_total = 0;
    wrote_total = 0;
    while (read_total < in_size) {
        int ch = fgetc(in);

        if (ch == EOF) {
            return -1;
        }
        read_total++;

        if ((unsigned char)ch == RLE_MARK) {
            int count = fgetc(in);
            int value = fgetc(in);
            unsigned char tmp[255];
            int i;

            if (count == EOF || value == EOF || read_total + 2 > in_size) {
                return -1;
            }
            read_total += 2;
            for (i = 0; i < count; i++) {
                tmp[i] = (unsigned char)value;
            }
            if (wrote_total + (uint64_t)count > out_size) {
                return -1;
            }
            if (out != NULL && fwrite(tmp, 1, (size_t)count, out) != (size_t)count) {
                return -1;
            }
            *crc = crc32_update(*crc, tmp, (size_t)count);
            wrote_total += (uint64_t)count;
        } else {
            unsigned char value = (unsigned char)ch;

            if (wrote_total + 1 > out_size) {
                return -1;
            }
            if (out != NULL && fwrite(&value, 1, 1, out) != 1) {
                return -1;
            }
            *crc = crc32_update(*crc, &value, 1);
            wrote_total++;
        }

        log_progress(opts, name, wrote_total, out_size, wrote_total == out_size);
    }

    return wrote_total == out_size ? 0 : -1;
}