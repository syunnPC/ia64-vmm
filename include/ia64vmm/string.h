/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_STRING_H
#define IA64VMM_STRING_H
#ifdef IA64VMM_HOSTED
#include <string.h>
#else
#include <stddef.h>
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
#endif
#endif
