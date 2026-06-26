#include "pak.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int flush_output(FILE *out, unsigned char *buf, size_t *len, uint32_t *crc)
{
    if (*len == 0) {
        return 0;
    }

    if (out != NULL && fwrite(buf, 1, *len, out) != *len) {
        return -1;
    }
    *crc = crc32_update(*crc, buf, *len);
    *len = 0;
    return 0;
}

static int append_output(FILE *out, unsigned char *buf, size_t *len, uint32_t *crc, const unsigned char *src, size_t src_len)
{
    size_t offset = 0;

    while (offset < src_len) {
        size_t room = COPY_BUF_SIZE - *len;
        size_t take = src_len - offset < room ? src_len - offset : room;

        memcpy(buf + *len, src + offset, take);
        *len += take;
        offset += take;

        if (*len == COPY_BUF_SIZE && flush_output(out, buf, len, crc) != 0) {
            return -1;
        }
    }

    return 0;
}

static int append_run(FILE *out, unsigned char *buf, size_t *len, uint32_t *crc, unsigned char value, int count)
{
    while (count > 0) {
        size_t room = COPY_BUF_SIZE - *len;
        size_t take = (size_t)count < room ? (size_t)count : room;

        memset(buf + *len, value, take);
        *len += take;
        count -= (int)take;

        if (*len == COPY_BUF_SIZE && flush_output(out, buf, len, crc) != 0) {
            return -1;
        }
    }

    return 0;
}

int rle_decompress(FILE *in, FILE *out, uint64_t in_size, uint64_t out_size, uint32_t *crc, const char *name, const struct pak_options *opts)
{
    unsigned char out_buf[COPY_BUF_SIZE];
    uint64_t read_total;
    uint64_t wrote_total;
    size_t out_len;

    read_total = 0;
    wrote_total = 0;
    out_len = 0;
    while (read_total < in_size) {
        int ch = fgetc(in);

        if (ch == EOF) {
            return -1;
        }
        read_total++;

        if ((unsigned char)ch == RLE_MARK) {
            int count = fgetc(in);
            int value = fgetc(in);

            if (count == EOF || value == EOF || read_total + 2 > in_size) {
                return -1;
            }
            read_total += 2;
            if (wrote_total + (uint64_t)count > out_size) {
                return -1;
            }
            if (append_run(out, out_buf, &out_len, crc, (unsigned char)value, count) != 0) {
                return -1;
            }
            wrote_total += (uint64_t)count;
        } else {
            unsigned char value = (unsigned char)ch;

            if (wrote_total + 1 > out_size) {
                return -1;
            }
            if (append_output(out, out_buf, &out_len, crc, &value, 1) != 0) {
                return -1;
            }
            wrote_total++;
        }

        log_progress(opts, name, wrote_total, out_size, wrote_total == out_size);
    }

    if (flush_output(out, out_buf, &out_len, crc) != 0) {
        return -1;
    }

    return wrote_total == out_size ? 0 : -1;
}
