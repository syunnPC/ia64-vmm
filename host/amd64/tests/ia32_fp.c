/* SPDX-License-Identifier: GPL-2.0-or-later */
/* APM Vol.2 section 11.4.4: FSAVE environment and ST(i), not physical order. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/ia32_svm.h"
#include "tests/host/test.h"
static uint64_t rd(const uint8_t *b, unsigned o, unsigned n)
{
    uint64_t x = 0;
    memcpy(&x, b + o, n);
    return x;
}
static void wr(uint8_t *b, unsigned o, uint64_t x, unsigned n)
{
    memcpy(b + o, &x, n);
}
static struct ia32_fp_image image(void)
{
    struct ia32_fp_image fp = {0};
    wr(fp.bytes, 0, 0x37f, 2);
    wr(fp.bytes, 2, 5 << 11, 2);
    fp.bytes[4] = 0xad;
    wr(fp.bytes, 6, 0x345, 2);
    wr(fp.bytes, 8, 0xabcdef01, 4);
    wr(fp.bytes, 12, 0x23, 2);
    wr(fp.bytes, 16, 0x12345678, 4);
    wr(fp.bytes, 20, 0x2b, 2);
    wr(fp.bytes, 24, 0x1f80, 4);
    for (unsigned i = 0; i < 8; ++i) {
        wr(fp.bytes, 32 + 16 * i, UINT64_C(0x8000000000000000) + i, 8);
        wr(fp.bytes, 40 + 16 * i, 0x3fff + i, 2);
        wr(fp.bytes, 160 + 16 * i, 0x12340000 + i, 8);
    }
    return fp;
}
static void format(void)
{
    struct ia32_fp_image fp = image(), out = fp;
    struct amd64_ia32_fsave fs;
    amd64_ia32_fp_pack(&fp, &fs);
    CHECK(rd(fs.bytes, 0, 2) == 0x37f && rd(fs.bytes, 4, 2) == (5 << 11));
    CHECK(rd(fs.bytes, 12, 4) == 0xabcdef01 && rd(fs.bytes, 16, 2) == 0x23 &&
          rd(fs.bytes, 18, 2) == 0x345);
    CHECK(rd(fs.bytes, 20, 4) == 0x12345678 && rd(fs.bytes, 24, 2) == 0x2b);
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(!memcmp(fs.bytes + 28 + 10 * i, fp.bytes + 32 + 16 * i, 10));
        CHECK(((rd(fs.bytes, 8, 2) >> (2 * i)) & 3) == ((fp.bytes[4] & (1u << i)) ? 0 : 3));
    }
    memset(out.bytes, 0xa5, 24);
    amd64_ia32_fp_unpack(&fs, &out);
    CHECK(!memcmp(out.bytes, fp.bytes, 5) && !memcmp(out.bytes + 6, fp.bytes + 6, 8));
    CHECK(!memcmp(out.bytes + 16, fp.bytes + 16, 6));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(!memcmp(out.bytes + 32 + 16 * i, fp.bytes + 32 + 16 * i, 10));
    CHECK(rd(out.bytes, 24, 4) == 0x1f80 && rd(out.bytes, 28, 4) == 0xffbf);
    for (unsigned i = 288; i < 512; ++i)
        CHECK(!out.bytes[i]);
    /* Physical FP5 is ST0 when TOP=5. Empty, zero, special and valid tags. */
    fp.bytes[4] = 0xff;
    wr(fp.bytes, 32, 0, 8);
    wr(fp.bytes, 40, 0, 2);
    wr(fp.bytes, 48, UINT64_C(0x8000000000000000), 8);
    wr(fp.bytes, 56, 0x7fff, 2);
    amd64_ia32_fp_pack(&fp, &fs);
    CHECK(((rd(fs.bytes, 8, 2) >> 10) & 3) == 1 && ((rd(fs.bytes, 8, 2) >> 12) & 3) == 2);
}
int main(void)
{
    format();
    puts("native FP format: full legacy environment, physical tags/TOP and reserved bytes passed");
    return 0;
}
