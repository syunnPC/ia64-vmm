/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_ACPI_TABLES_H
#define VMM_ACPI_TABLES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ia64vmm/platform.h>
#include "../../console-tables.h"

/* ACPI 3.0b layouts; byte access also works before unaligned recovery exists. */
#define ACPI_IMAGE_CAPACITY 32768u
#define TABLE_MAX_CPUS 256u
struct acpi_image {
    uint8_t bytes[ACPI_IMAGE_CAPACITY];
    size_t used, rsdp, xsdt, fadt, dsdt, madt, mcfg, srat, slit;
    /* Optional console contributions; SIZE_MAX denotes an absent description. */
    size_t uart, spcr, hcdp;
};
static inline void table_zero(void *buffer, size_t size)
{
    /* GCC's loop idiom pass may emit libc memset even with -fno-builtin. */
    volatile uint8_t *p = buffer;
    while (size--)
        *p++ = 0;
}
static inline void table_put(uint8_t *p, uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}
static inline uint64_t table_get(const uint8_t *p, unsigned size)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static inline void table_text(uint8_t *p, const char *s, size_t size)
{
    while (size--)
        *p++ = (uint8_t)*s++;
}
static inline uint8_t table_sum(const uint8_t *p, size_t size)
{
    uint8_t sum = 0;
    while (size--)
        sum = (uint8_t)(sum + *p++);
    return sum;
}
bool acpi_build_dsdt(const struct ia64vmm_handoff *h, uint8_t *buffer, size_t capacity,
                     size_t *length);
/* memory_map is the UEFI GetMemoryMap byte array, with its returned stride. */
bool acpi_build(const struct ia64vmm_handoff *h, struct acpi_image *image, uint64_t physical_base,
                uint8_t facs[64], uint64_t facs_base, const void *memory_map, size_t map_size,
                size_t descriptor_size);
#endif
