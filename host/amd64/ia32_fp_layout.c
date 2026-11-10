/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/ia32_svm.h"
#include "ia64vmm/string.h"
static uint64_t get(const uint8_t *p, unsigned off, unsigned n)
{
    uint64_t v = 0;
    memcpy(&v, p + off, n);
    return v;
}
static void put(uint8_t *p, unsigned off, uint64_t v, unsigned n)
{
    memcpy(p + off, &v, n);
}
void amd64_ia32_fp_pack(const struct ia32_fp_image *fp, struct amd64_ia32_fsave *fs)
{
    const uint8_t *p = fp->bytes;
    uint8_t *s = fs->bytes;
    memset(s, 0, sizeof(fs->bytes));
    put(s, 0, get(p, 0, 2), 2);
    put(s, 4, get(p, 2, 2), 2);
    put(s, 12, get(p, 8, 4), 4);
    put(s, 16, get(p, 12, 2), 2);
    put(s, 18, get(p, 6, 2) & 0x7ff, 2);
    put(s, 20, get(p, 16, 4), 4);
    put(s, 24, get(p, 20, 2), 2);
    unsigned top = (get(p, 2, 2) >> 11) & 7, tags = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const uint8_t *r = p + 32 + 16 * i;
        unsigned physical = (top + i) & 7, tag = 3;
        if (p[4] & (1u << physical)) {
            unsigned exp = get(r, 8, 2) & 0x7fff;
            uint64_t sig = get(r, 0, 8);
            tag = (!exp && !sig) ? 1 : (!exp || exp == 0x7fff || !(sig >> 63)) ? 2 : 0;
        }
        tags |= tag << (2 * physical);
        memcpy(s + 28 + 10 * i, r, 10);
    }
    put(s, 8, tags, 2);
}
void amd64_ia32_fp_unpack(const struct amd64_ia32_fsave *fs, struct ia32_fp_image *fp)
{
    const uint8_t *s = fs->bytes;
    uint8_t *p = fp->bytes;
    put(p, 0, get(s, 0, 2), 2);
    put(p, 2, get(s, 4, 2), 2);
    p[4] = 0;
    p[5] = 0;
    unsigned tags = get(s, 8, 2);
    for (unsigned i = 0; i < 8; ++i)
        if (((tags >> (2 * i)) & 3) != 3)
            p[4] |= 1u << i;
    put(p, 6, get(s, 18, 2) & 0x7ff, 2);
    put(p, 8, get(s, 12, 4), 4);
    put(p, 12, get(s, 16, 2), 2);
    put(p, 14, 0, 2);
    put(p, 16, get(s, 20, 4), 4);
    put(p, 20, get(s, 24, 2), 2);
    put(p, 22, 0, 2);
    put(p, 28, 0xffbf, 4);
    for (unsigned i = 0; i < 8; ++i) {
        memcpy(p + 32 + 16 * i, s + 28 + 10 * i, 10);
        memset(p + 42 + 16 * i, 0, 6);
    }
    memset(p + 288, 0, 224); /* No host XMM8-15 or undefined padding in legacy state. */
}
