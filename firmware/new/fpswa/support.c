/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
/* Local compiler support; no EFI or host call may enter the runtime kernel. */
void *memset(void *dst, int value, size_t count)
{
    unsigned char *p = dst;
    for (size_t k = 0; k < count; ++k)
        p[k] = (unsigned char)value;
    return dst;
}
void *CopyMem(void *dst, const void *src, size_t count)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t k = 0; k < count; ++k)
        d[k] = s[k];
    return dst;
}
