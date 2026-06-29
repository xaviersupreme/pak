#include "pak.h"

static uint32_t crc_table[256];
static int crc_ready;

static void crc32_init(void)
{
    uint32_t c;
    int i;
    int bit;

    if (crc_ready) {
        return;
    }

    for (i = 0; i < 256; i++) {
        c = (uint32_t)i;
        for (bit = 0; bit < 8; bit++) {
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[i] = c;
    }

    crc_ready = 1;
}

uint32_t crc32_start(void)
{
    crc32_init();
    return 0xffffffffu;
}

uint32_t crc32_update(uint32_t crc, const unsigned char *buf, size_t size)
{
    size_t i;

    crc32_init();
    for (i = 0; i < size; i++) {
        crc = crc_table[(crc ^ buf[i]) & 0xffu] ^ (crc >> 8);
    }

    return crc;
}

uint32_t crc32_finish(uint32_t crc)
{
    return crc ^ 0xffffffffu;
}