/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "test.h"
#include "ia64vmm/log.h"

static void compare(const char *fmt, ...)
{
    static const size_t sizes[] = {0, 1, 2, 7, 64, 512};
    va_list ap;
    va_start(ap, fmt);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        unsigned char actual[514], expected[514];
        memset(actual, 0xa5, sizeof(actual));
        memset(expected, 0xa5, sizeof(expected));
        va_list a, b;
        va_copy(a, ap);
        va_copy(b, ap);
        int got = vmm_vsnprintf((char *)actual + 1, sizes[i], fmt, a);
        int want = vsnprintf((char *)expected + 1, sizes[i], fmt, b);
        va_end(a);
        va_end(b);
        CHECK(got == want);
        CHECK(!memcmp(actual, expected, sizeof(actual)));
    }
    va_end(ap);
}

/* The project deliberately supports a null %s; libc's format attribute
 * diagnoses that extension, so exercise it through the va_list entry. */
static int unchecked_format(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int result = vmm_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return result;
}

void test_format(void)
{
    compare("");
    compare("plain text with newline\n");
    compare("%s|%c|%%|%d|%i|%u|%x|%X", "hello", 'A', -123, 456, 789u, 0xabcdu, 0xabcdu);
    compare("%s|%c|%u", "", 0, 0u);
    compare("%10s|%-10s|%4c|%-4c", "text", "text", 'X', 'Y');
    compare("%d %i %u", INT_MIN, INT_MAX, UINT_MAX);
    compare("%ld|%li|%lu|%lx|%lX", LONG_MIN, LONG_MAX, ULONG_MAX, ULONG_MAX, ULONG_MAX);
    compare("%lld|%lli|%llu|%llx|%llX", LLONG_MIN, LLONG_MAX, ULLONG_MAX, ULLONG_MAX, ULLONG_MAX);
    compare("%zd|%zi|%zu|%zx|%zX", (ptrdiff_t)PTRDIFF_MIN, (ptrdiff_t)PTRDIFF_MAX, (size_t)SIZE_MAX,
            (size_t)SIZE_MAX, (size_t)SIZE_MAX);
    compare("%12d|%-12d|%012d|%-012d", -123, -123, -123, -123);
    compare("%12i|%-12i|%012i|%-012i", 123, 123, 123, 123);
    compare("%12u|%-12u|%012u|%-012u", 123u, 123u, 123u, 123u);
    compare("%12x|%-12x|%012x|%-012x", 0xabu, 0xabu, 0xabu, 0xabu);
    compare("%12X|%-12X|%012X|%-012X", 0xabu, 0xabu, 0xabu, 0xabu);
    compare("%2d|%02d|%-2d", -1234, -1234, -1234);
    compare("%020lld|%020llu", LLONG_MIN, ULLONG_MAX);
    compare("%p|%20p|%-20p|%020p", (void *)(uintptr_t)0x1234, (void *)(uintptr_t)0x1234,
            (void *)(uintptr_t)0x1234, (void *)(uintptr_t)0x1234);
    char buf[64];
    CHECK(vmm_snprintf(buf, sizeof(buf), "%p", (void *)NULL) == 3);
    CHECK(!strcmp(buf, "0x0"));
    const char *null_string = NULL;
    const char *string_format = "%s";
    CHECK(unchecked_format(buf, sizeof(buf), string_format, null_string) == 6);
    CHECK(!strcmp(buf, "(null)"));
    CHECK(vmm_snprintf(NULL, 0, "%s %d", "hello", 42) == 8);
    CHECK(vmm_snprintf(buf, 4, "%2147483647u", 1u) == INT_MAX);
    CHECK(!strcmp(buf, "   "));
}
