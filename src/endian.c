#include "pak.h"

#include <stdint.h>
#include <stdio.h>

int write_u32_le(FILE *fp, uint32_t value)
{
    unsigned char b[4];

    b[0] = (unsigned char)(value & 0xffu);
    b[1] = (unsigned char)((value >> 8) & 0xffu);
    b[2] = (unsigned char)((value >> 16) & 0xffu);
    b[3] = (unsigned char)((value >> 24) & 0xffu);

    return fwrite(b, 1, sizeof(b), fp) == sizeof(b) ? 0 : -1;
}

int write_u64_le(FILE *fp, uint64_t value)
{
    unsigned char b[8];
    int i;

    for (i = 0; i < 8; i++) {
        b[i] = (unsigned char)((value >> (i * 8)) & 0xffu);
    }

    return fwrite(b, 1, sizeof(b), fp) == sizeof(b) ? 0 : -1;
}

int read_u32_le(FILE *fp, uint32_t *value)
{
    unsigned char b[4];

    if (fread(b, 1, sizeof(b), fp) != sizeof(b)) {
        return -1;
    }

    *value = ((uint32_t)b[0]) |
             ((uint32_t)b[1] << 8) |
             ((uint32_t)b[2] << 16) |
             ((uint32_t)b[3] << 24);
    return 0;
}

int read_u64_le(FILE *fp, uint64_t *value)
{
    unsigned char b[8];
    uint64_t out;
    int i;

    if (fread(b, 1, sizeof(b), fp) != sizeof(b)) {
        return -1;
    }

    out = 0;
    for (i = 0; i < 8; i++) {
        out |= ((uint64_t)b[i]) << (i * 8);
    }
    *value = out;
    return 0;
}
