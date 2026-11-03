/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host structural test: poisoned input, holes, buffer bounds, PI list walk. */
#include <assert.h>
#include <string.h>
#undef NULL
#include "firmware/new/hob.h"

/* Legacy VGA: the legacy tail is validated
 * (firmware/new/legacy-handoff.h; every rule: tests/firmware/legacy_bios.c),
 * page 0 becomes a reserved allocation and C0000-FFFFF an uncached MMIO
 * resource. Without legacy VGA the HOBs also describe the aperture and reserve page 0 by default. */
struct legacy_hobs {
    unsigned area, page0, vga;
};
static struct legacy_hobs walk_legacy(const UINT8 *at)
{
    struct legacy_hobs found = {0, 0, 0};
    while (((const EFI_HOB_GENERIC_HEADER *)at)->HobType != EFI_HOB_TYPE_END_OF_HOB_LIST) {
        const EFI_HOB_GENERIC_HEADER *head = (const VOID *)at;
        if (head->HobType == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR) {
            const EFI_HOB_RESOURCE_DESCRIPTOR *r = (const VOID *)at;
            if (r->PhysicalStart == IA64VMM_LEGACY_AREA_BASE)
                found.area += r->ResourceType == EFI_RESOURCE_MEMORY_MAPPED_IO &&
                                      r->ResourceLength == IA64VMM_LEGACY_AREA_SIZE &&
                                      (r->ResourceAttribute & EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE)
                                  ? 1
                                  : 100;
            if (r->PhysicalStart == IA64VMM_VGA_MEMORY_BASE)
                found.vga += r->ResourceType == EFI_RESOURCE_MEMORY_MAPPED_IO &&
                                     r->ResourceLength == IA64VMM_VGA_MEMORY_SIZE
                                 ? 1
                                 : 100;
        }
        if (head->HobType == EFI_HOB_TYPE_MEMORY_ALLOCATION) {
            const EFI_HOB_MEMORY_ALLOCATION *a = (const VOID *)at;
            if (a->AllocDescriptor.MemoryBaseAddress == 0)
                found.page0 += a->AllocDescriptor.MemoryLength == 4096 &&
                                       a->AllocDescriptor.MemoryType == EfiReservedMemoryType
                                   ? 1
                                   : 100;
        }
        at += head->HobLength;
    }
    return found;
}
static void test_legacy(const struct ia64vmm_handoff *base, UINT64 core_base, UINT64 entry)
{
    static _Alignas(8) UINT8 buffer[IPL_HOB_CAPACITY];
    struct ia64vmm_handoff h = *base;
    h.ram_range_count = 4;
    h.ram[3] = h.ram[2];
    h.ram[2] = h.ram[1];
    h.ram[1] = h.ram[0];
    h.ram[0] = (struct ia64vmm_ram_range){0, 0xa0000};
    h.flags |= IA64VMM_HF_LEGACY_VGA;
    h.vga_bdf = 0x0010;
    h.vga_base = IA64VMM_VGA_MEMORY_BASE;
    h.vga_size = IA64VMM_VGA_MEMORY_SIZE;
    h.fb_base = h.fb_size = 0;
    h.fb_format = IA64VMM_FB_NONE;
    /* Default path: the same legacy resources and page-zero reservation. */
    assert(ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    assert(
        ipl_build_hobs(&h, buffer, IPL_HOB_CAPACITY, core_base, 0x20000, entry, 0x110000, 0x10000));
    struct legacy_hobs found = walk_legacy(buffer);
    assert(found.area == 1 && found.page0 == 1 && found.vga == 1);
    /* Unclaimed PCI legacy space is still MMIO (master abort). */
    h.flags &= ~IA64VMM_HF_LEGACY_VGA;
    h.vga_base = h.vga_size = 0;
    assert(
        ipl_build_hobs(&h, buffer, IPL_HOB_CAPACITY, core_base, 0x20000, entry, 0x110000, 0x10000));
    found = walk_legacy(buffer);
    assert(found.area == 1 && found.page0 == 1 && found.vga == 1);
    /* Match the reference's guards when these addresses really are RAM. */
    h.ram[0].size = 0x100000;
    assert(
        ipl_build_hobs(&h, buffer, IPL_HOB_CAPACITY, core_base, 0x20000, entry, 0x110000, 0x10000));
    found = walk_legacy(buffer);
    assert(!found.area && !found.vga && found.page0 == 1);
    h.ram[0].size = 0xa0000;
    h.flags |= IA64VMM_HF_LEGACY_VGA;
    h.vga_base = IA64VMM_VGA_MEMORY_BASE;
    h.vga_size = IA64VMM_VGA_MEMORY_SIZE;
    h.legacy_version = IA64VMM_HANDOFF_LEGACY_VERSION;
    h.legacy_bdf = 0x0010;
    h.legacy_flags =
        IA64VMM_LEGACY_TEXT_CONSOLE | IA64VMM_LEGACY_IVT | IA64VMM_LEGACY_ROM | IA64VMM_LEGACY_STUB;
    h.legacy_ivt_base = IA64VMM_LEGACY_IVT_BASE;
    h.legacy_ivt_size = IA64VMM_LEGACY_IVT_SIZE;
    h.legacy_rom_base = IA64VMM_LEGACY_ROM_BASE;
    h.legacy_rom_size = 0x8000;
    h.legacy_stub_base = IA64VMM_LEGACY_STUB_BASE;
    h.legacy_stub_size = IA64VMM_LEGACY_STUB_SIZE;
    assert(ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    assert(
        ipl_build_hobs(&h, buffer, IPL_HOB_CAPACITY, core_base, 0x20000, entry, 0x110000, 0x10000));
    found = walk_legacy(buffer);
    assert(found.area == 1 && found.page0 == 1 && found.vga == 1);
    /* IVT copying is optional; the page-zero reservation is unconditional. */
    h.legacy_flags &= ~IA64VMM_LEGACY_IVT;
    h.legacy_ivt_size = 0;
    assert(ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    assert(
        ipl_build_hobs(&h, buffer, IPL_HOB_CAPACITY, core_base, 0x20000, entry, 0x110000, 0x10000));
    found = walk_legacy(buffer);
    assert(found.area == 1 && found.page0);
    h.legacy_flags |= IA64VMM_LEGACY_IVT;
    h.legacy_ivt_size = IA64VMM_LEGACY_IVT_SIZE;
    /* Broken tails: the IPL refuses the handoff. */
    struct ia64vmm_handoff bad = h;
    bad.legacy_flags |= 0x80000000u; /* bad-flags */
    assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
    bad = h;
    bad.legacy_rom_size = 511; /* bad-rom */
    assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
    bad = h;
    bad.flags &= ~IA64VMM_HF_LEGACY_VGA; /* no-vga */
    assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
    bad = h;
    bad.legacy_version = 2; /* bad-version */
    assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
    bad = h;
    bad.fb_format = IA64VMM_FB_BGRX8888;
    bad.fb_base = 0x80000000;
    bad.fb_size = 0x300000;
    assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
    bad = h;
    bad.ram[0].base = 0x1000;
    bad.ram[0].size = 0x9f000; /* page 0 not RAM */
    assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
    bad = base[0];
    bad.legacy_reserved[1] = 1; /* stray field, version 0 */
    assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
}

static void test_partial_windows(const struct ia64vmm_handoff *original, UINT64 core, UINT64 entry)
{
    static _Alignas(8) UINT8 buffer[IPL_HOB_CAPACITY];
    struct ia64vmm_handoff h = *original;
    /* Disjoint low RAM islands and BARs exercise both ends and inner holes. */
    h.ram_range_count = 6;
    h.ram[5] = h.ram[2];
    h.ram[4] = h.ram[1];
    h.ram[3] = h.ram[0];
    h.ram[0] = (struct ia64vmm_ram_range){0, 0x9e000};
    h.ram[1] = (struct ia64vmm_ram_range){0xa8000, 0x4000};
    h.ram[2] = (struct ia64vmm_ram_range){0xd0000, 0x4000};
    h.pci_mmio_range_count = 2;
    h.pci_mmio[0] = (struct ia64vmm_ram_range){0xb0000, 0x2000};
    h.pci_mmio[1] = (struct ia64vmm_ram_range){0xc8000, 0x2000};
    assert(ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    assert(ipl_build_hobs(&h, buffer, sizeof(buffer), core, 0x20000, entry, 0x110000, 0x10000));
    for (UINT64 page = 0xa0000; page < 0x100000; page += 4096) {
        unsigned ram = 0, mmio = 0;
        for (const UINT8 *at = buffer;
             ((const EFI_HOB_GENERIC_HEADER *)at)->HobType != EFI_HOB_TYPE_END_OF_HOB_LIST;
             at += ((const EFI_HOB_GENERIC_HEADER *)at)->HobLength) {
            if (((const EFI_HOB_GENERIC_HEADER *)at)->HobType != EFI_HOB_TYPE_RESOURCE_DESCRIPTOR)
                continue;
            const EFI_HOB_RESOURCE_DESCRIPTOR *r = (const VOID *)at;
            if (page < r->PhysicalStart || page - r->PhysicalStart >= r->ResourceLength)
                continue;
            ram += r->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY;
            mmio += r->ResourceType == EFI_RESOURCE_MEMORY_MAPPED_IO;
        }
        assert(ram + mmio == 1);
    }
}

static void test_ram_granules(const struct ia64vmm_handoff *original, UINT64 core, UINT64 entry)
{
    static _Alignas(8) UINT8 buffer[IPL_HOB_CAPACITY];
    unsigned original_runs = 1;
    for (unsigned i = 1; i < original->ram_range_count; ++i)
        original_runs +=
            original->ram[i - 1].base + original->ram[i - 1].size != original->ram[i].base;
    /* Exercise both odd edges and intervals too small for a full granule.
     * Required firmware/data allocations stay in the unmodified ranges. */
    for (UINT64 base = 0x1000; base < 0x20000; base += 4096) {
        for (UINT64 size = 4096; size <= 0x10000; size += 4096) {
            struct ia64vmm_handoff h = *original;
            for (unsigned i = h.ram_range_count; i; --i)
                h.ram[i] = h.ram[i - 1];
            ++h.ram_range_count;
            h.ram[0] = (struct ia64vmm_ram_range){base, size};
            struct ia64vmm_handoff saved = h;
            assert(ipl_build_hobs(&h, buffer, sizeof(buffer), core, 0x20000, entry, 0x110000,
                                  0x10000));
            assert(!memcmp(&saved, &h, sizeof(h)));
            unsigned resources = 0, low = 0;
            for (const UINT8 *at = buffer;
                 ((const EFI_HOB_GENERIC_HEADER *)at)->HobType != EFI_HOB_TYPE_END_OF_HOB_LIST;
                 at += ((const EFI_HOB_GENERIC_HEADER *)at)->HobLength) {
                if (((const EFI_HOB_GENERIC_HEADER *)at)->HobType !=
                    EFI_HOB_TYPE_RESOURCE_DESCRIPTOR)
                    continue;
                const EFI_HOB_RESOURCE_DESCRIPTOR *r = (const VOID *)at;
                if (r->ResourceType != EFI_RESOURCE_SYSTEM_MEMORY)
                    continue;
                ++resources;
                assert(r->PhysicalStart % 8192 == 0 && r->ResourceLength &&
                       r->ResourceLength % 8192 == 0);
                if (r->PhysicalStart >= 0x100000)
                    continue;
                ++low;
                UINT64 start = base;
                while (start % 8192)
                    start += 4096;
                UINT64 end = base + size;
                while (end % 8192)
                    end -= 4096;
                assert(r->PhysicalStart == start && r->ResourceLength == end - start);
                assert(r->PhysicalStart >= base &&
                       r->PhysicalStart + r->ResourceLength <= base + size);
            }
            /* Independently count complete 8 KiB pages within the input. */
            unsigned complete = 0;
            for (UINT64 page = 0; page < base + size; page += 8192)
                complete += page >= base && page + 8192 <= base + size;
            assert(low == (complete != 0));
            assert(resources == original_runs + low);
        }
    }
}

/* Describe RAM in whole 8 KiB granules without covering adjacent reserved
 * pages when a range ends on an odd 4 KiB boundary. */
static unsigned described_pages(const UINT8 *buffer, UINT64 page, BOOLEAN allocation)
{
    unsigned n = 0;
    for (const UINT8 *at = buffer;
         ((const EFI_HOB_GENERIC_HEADER *)at)->HobType != EFI_HOB_TYPE_END_OF_HOB_LIST;
         at += ((const EFI_HOB_GENERIC_HEADER *)at)->HobLength) {
        const EFI_HOB_GENERIC_HEADER *head = (const VOID *)at;
        UINT64 base, length;
        if (!allocation && head->HobType == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR &&
            ((const EFI_HOB_RESOURCE_DESCRIPTOR *)at)->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) {
            base = ((const EFI_HOB_RESOURCE_DESCRIPTOR *)at)->PhysicalStart;
            length = ((const EFI_HOB_RESOURCE_DESCRIPTOR *)at)->ResourceLength;
        } else if (allocation && head->HobType == EFI_HOB_TYPE_MEMORY_ALLOCATION) {
            base = ((const EFI_HOB_MEMORY_ALLOCATION *)at)->AllocDescriptor.MemoryBaseAddress;
            length = ((const EFI_HOB_MEMORY_ALLOCATION *)at)->AllocDescriptor.MemoryLength;
        } else
            continue;
        n += page >= base && page - base < length;
    }
    return n;
}
static BOOLEAN raw_ram(const struct ia64vmm_handoff *h, UINT64 page)
{
    for (unsigned i = 0; i < h->ram_range_count; ++i)
        if (page >= h->ram[i].base && page - h->ram[i].base < h->ram[i].size)
            return TRUE;
    return FALSE;
}
static void test_granules(const struct ia64vmm_handoff *original, UINT64 core, UINT64 entry)
{
    static _Alignas(8) UINT8 buffer[IPL_HOB_CAPACITY];
    static const struct ia64vmm_ram_range pc3072[] = {{0, 0xa0000},
                                                      {0x100000, 0x700000},
                                                      {0x808000, 0x3000},
                                                      {0x80c000, 0x5000},
                                                      {0x900000, 0xb046c000},
                                                      {0xb8d6c000, 0x848000},
                                                      {0xb9b31000, 0x52fc000},
                                                      {0xbeeee000, 0x9ff000},
                                                      {0xbfbff000, 0x281000},
                                                      {0xbfe86000, 0x56000}};
    struct ia64vmm_handoff h = *original;
    h.ram_range_count = sizeof(pc3072) / sizeof(pc3072[0]);
    memcpy(h.ram, pc3072, sizeof(pc3072));
    h.firmware_base = 0x900000;
    h.firmware_size = 0x82c000;
    h.fw_data_base = 0xbce20000;
    h.fw_data_size = 0x2000000;
    h.boot_image_base = 0xbfe86000;
    h.boot_image_size = 0x15001;
    core = h.fw_data_base + IPL_CORE_OFFSET;
    entry = core + 0x1000;
    assert(ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    assert(ipl_build_hobs(&h, buffer, sizeof(buffer), core, 0x20000, entry, 0x910000, 0x10000));
    /* Each 4 KiB page is RAM in the HOBs exactly when its whole 8 KiB granule
     * is handoff RAM, and every allocation lies in described RAM. */
    for (UINT64 page = 0; page < 0xc0000000; page += 4096) {
        UINT64 granule = page & ~(UINT64)0x1fff;
        unsigned ram = described_pages(buffer, page, FALSE);
        assert(ram == (raw_ram(&h, granule) && raw_ram(&h, granule + 4096) ? 1u : 0u));
        assert(!described_pages(buffer, page, TRUE) || ram == 1);
    }
    for (const UINT8 *at = buffer;
         ((const EFI_HOB_GENERIC_HEADER *)at)->HobType != EFI_HOB_TYPE_END_OF_HOB_LIST;
         at += ((const EFI_HOB_GENERIC_HEADER *)at)->HobLength) {
        const EFI_HOB_RESOURCE_DESCRIPTOR *r = (const VOID *)at;
        if (r->Header.HobType == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR &&
            r->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY)
            assert(r->PhysicalStart % 0x2000 == 0 && r->ResourceLength % 0x2000 == 0);
    }
    /* Granules containing a hole or a reserved page are not RAM. */
    static const UINT64 absent[] = {0x80a000,   0x80b000,   0x810000,   0xb9b30000, 0xb9b31000,
                                    0xbee2c000, 0xbf8ec000, 0xbfbfe000, 0xbfbff000};
    static const UINT64 present[] = {0x0,        0x808000,   0x80e000,  0xb9b32000,
                                     0xbee2a000, 0xbf8ea000, 0xbfc00000};
    for (unsigned i = 0; i < sizeof(absent) / sizeof(absent[0]); ++i)
        assert(!described_pages(buffer, absent[i], FALSE));
    for (unsigned i = 0; i < sizeof(present) / sizeof(present[0]); ++i)
        assert(described_pages(buffer, present[i], FALSE) == 1);
    /* Adjacent handoff ranges meeting on an odd 4 KiB boundary are one run. */
    struct ia64vmm_handoff joined = h;
    joined.ram[2] = (struct ia64vmm_ram_range){0x808000, 0x1000};
    joined.ram[3] = (struct ia64vmm_ram_range){0x809000, 0x1000};
    assert(ipl_validate_handoff(&joined, sizeof(joined), IA64VMM_PLATFORM_ID));
    assert(
        ipl_build_hobs(&joined, buffer, sizeof(buffer), core, 0x20000, entry, 0x910000, 0x10000));
    assert(described_pages(buffer, 0x808000, FALSE) == 1 &&
           described_pages(buffer, 0x809000, FALSE) == 1);
    /* An allocation in raw RAM but outside the granules is refused. */
    struct ia64vmm_handoff bad = h;
    bad.boot_image_base = 0xbf8ec000;
    bad.boot_image_size = 0x1000;
    assert(ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
    assert(!ipl_build_hobs(&bad, buffer, sizeof(buffer), core, 0x20000, entry, 0x910000, 0x10000));
    bad = h;
    bad.ram[0] = (struct ia64vmm_ram_range){0, 0x1000}; /* page 0 alone: not described */
    assert(ipl_build_hobs(&bad, buffer, sizeof(buffer), core, 0x20000, entry, 0x910000, 0x10000));
    assert(!described_pages(buffer, 0, FALSE) && !described_pages(buffer, 0, TRUE));
}

int main(void)
{
    struct ia64vmm_handoff h = {0};
    ia64vmm_handoff_cpu_model(&h, IA64_CPU_MONTECITO);
    h.magic = IA64VMM_HANDOFF_MAGIC;
    h.version = IA64VMM_HANDOFF_VERSION;
    h.size = sizeof(h);
    h.cpu_count = 1;
    h.itc_hz = IA64VMM_ITC_HZ;
    h.service_base = IA64VMM_SERVICE_BASE;
    h.service_size = IA64VMM_SERVICE_SIZE;
    h.ram_range_count = 2;
    h.ram[0] = (struct ia64vmm_ram_range){0x100000, 0x700000};
    h.ram[1] = (struct ia64vmm_ram_range){0x900000, 0x7000000};
    h.firmware_base = 0x100000;
    h.firmware_size = 0x100000;
    h.fw_data_base = 0x1000000;
    h.fw_data_size = 0x2000000;
    h.boot_image_base = 0x4000000;
    h.boot_image_size = 0x10000;
    static _Alignas(8) UINT8 buffer[IPL_HOB_CAPACITY + 8];
    memset(buffer, 0xdd, sizeof(buffer));
    UINT64 core_base = h.fw_data_base + IPL_CORE_OFFSET;
    UINT64 entry = core_base + 0x1000;
    assert(ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    h.cpu_count = IA64VMM_MAX_CPUS;
    assert(ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    h.cpu_count = IA64VMM_MAX_CPUS + 1;
    assert(!ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    h.cpu_count = 1;
    assert(!ipl_validate_handoff(&h, sizeof(h) - 1, IA64VMM_PLATFORM_ID));
    assert(!ipl_validate_handoff(&h, sizeof(h), 0));
    assert(!ipl_build_hobs(&h, buffer, 8, core_base, 0x20000, entry, 0x110000, 0x10000));
    assert(!ipl_build_hobs(&h, buffer, IPL_HOB_CAPACITY, core_base, 1, entry, 0x110000, 0x10000));
    assert(ipl_build_hobs(&h, buffer, IPL_HOB_CAPACITY, core_base, 0x20000, entry, 0x110000,
                          0x10000) == buffer);
    EFI_HOB_HANDOFF_INFO_TABLE *phit = (VOID *)buffer;
    assert(phit->Version == EFI_HOB_HANDOFF_TABLE_VERSION);
    assert(phit->EfiFreeMemoryBottom == h.fw_data_base + IPL_FREE_OFFSET);
    assert(phit->EfiFreeMemoryTop == h.fw_data_base + h.fw_data_size);
    unsigned mmio = 0, resources = 0, cpus = 0, fvs = 0, modules = 0, private = 0;
    UINT8 *at = buffer;
    UINT64 allocations[16][2];
    unsigned count = 0;
    while (((EFI_HOB_GENERIC_HEADER *)at)->HobType != EFI_HOB_TYPE_END_OF_HOB_LIST) {
        EFI_HOB_GENERIC_HEADER *head = (VOID *)at;
        assert(head->HobLength >= 8 && head->HobLength % 8 == 0);
        assert(at + head->HobLength <= buffer + IPL_HOB_CAPACITY);
        assert(head->Reserved == 0);
        if (head->HobType == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR) {
            EFI_HOB_RESOURCE_DESCRIPTOR *r = (VOID *)at;
            if (r->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) {
                assert(resources < 2 && r->PhysicalStart == h.ram[resources].base &&
                       r->ResourceLength == h.ram[resources].size);
                assert(r->ResourceAttribute & EFI_RESOURCE_ATTRIBUTE_TESTED);
                assert(r->ResourceAttribute & EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE);
                assert(!(r->ResourceAttribute & EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE));
                ++resources;
            } else {
                assert(r->ResourceType == EFI_RESOURCE_MEMORY_MAPPED_IO);
                assert(
                    (r->PhysicalStart == h.service_base && r->ResourceLength == h.service_size) ||
                    (r->PhysicalStart == IA64VMM_VGA_MEMORY_BASE &&
                     r->ResourceLength == IA64VMM_VGA_MEMORY_SIZE) ||
                    (r->PhysicalStart == IA64VMM_LEGACY_AREA_BASE &&
                     r->ResourceLength == IA64VMM_LEGACY_AREA_SIZE));
                assert(r->ResourceAttribute & EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE);
                ++mmio;
            }
        }
        if (head->HobType == EFI_HOB_TYPE_MEMORY_ALLOCATION) {
            EFI_HOB_MEMORY_ALLOCATION *a = (VOID *)at;
            assert(count < 16 && a->AllocDescriptor.MemoryBaseAddress % 4096 == 0 &&
                   a->AllocDescriptor.MemoryLength % 4096 == 0);
            allocations[count][0] = a->AllocDescriptor.MemoryBaseAddress;
            allocations[count++][1] = a->AllocDescriptor.MemoryLength;
            if (head->HobLength == sizeof(EFI_HOB_MEMORY_ALLOCATION_MODULE)) {
                EFI_HOB_MEMORY_ALLOCATION_MODULE *m = (VOID *)at;
                assert(m->EntryPoint == entry &&
                       m->MemoryAllocationHeader.MemoryBaseAddress == core_base);
                ++modules;
            }
        }
        if (head->HobType == EFI_HOB_TYPE_CPU) {
            EFI_HOB_CPU *cpu = (VOID *)at;
            assert(cpu->SizeOfMemorySpace == 50 && cpu->SizeOfIoSpace == 16);
            ++cpus;
        }
        if (head->HobType == EFI_HOB_TYPE_FV)
            ++fvs;
        if (head->HobType == EFI_HOB_TYPE_GUID_EXTENSION) {
            assert(memcmp(at + sizeof(EFI_HOB_GUID_TYPE), &h, sizeof(h)) == 0);
            ++private;
        }
        at += head->HobLength;
    }
    assert(phit->EfiEndOfHobList == (UINTN)at);
    assert(mmio == 3 && resources == 2 && cpus == 1 && fvs == 1 && modules == 1 && private == 1);
    for (unsigned i = 0; i < count; ++i)
        for (unsigned j = i + 1; j < count; ++j)
            assert(allocations[i][0] + allocations[i][1] <= allocations[j][0] ||
                   allocations[j][0] + allocations[j][1] <= allocations[i][0]);
    for (unsigned i = IPL_HOB_CAPACITY; i < sizeof(buffer); ++i)
        assert(buffer[i] == 0xdd);
    h.pci_mmio_range_count = 1;
    h.pci_mmio[0] = (struct ia64vmm_ram_range){0x80000000, 0x200000};
    h.fb_base = 0x80010000;
    h.fb_size = 0x10000;
    h.ecam_base = IA64VMM_ECAM_BASE;
    h.ecam_size = IA64VMM_ECAM_SIZE;
    h.legacy_io_base = IA64VMM_LEGACY_IO_BASE;
    h.legacy_io_size = IA64VMM_LEGACY_IO_SIZE;
    assert(
        ipl_build_hobs(&h, buffer, IPL_HOB_CAPACITY, core_base, 0x20000, entry, 0x110000, 0x10000));
    at = buffer;
    unsigned assigned = 0, framebuffer = 0, windows = 0;
    while (((EFI_HOB_GENERIC_HEADER *)at)->HobType != EFI_HOB_TYPE_END_OF_HOB_LIST) {
        EFI_HOB_GENERIC_HEADER *head = (VOID *)at;
        if (head->HobType == EFI_HOB_TYPE_RESOURCE_DESCRIPTOR) {
            EFI_HOB_RESOURCE_DESCRIPTOR *r = (VOID *)at;
            if (r->PhysicalStart == h.pci_mmio[0].base) {
                assert(r->ResourceType == EFI_RESOURCE_MEMORY_MAPPED_IO &&
                       r->ResourceLength == 0x200000);
                ++assigned;
            }
            if (r->PhysicalStart == h.fb_base)
                ++framebuffer;
            if (r->PhysicalStart == h.ecam_base || r->PhysicalStart == h.legacy_io_base)
                ++windows;
        }
        at += head->HobLength;
    }
    assert(assigned == 1 && framebuffer == 0 && windows == 2);
    h.pci_mmio_range_count = IA64VMM_HANDOFF_MAX_PCI_MMIO + 1;
    assert(!ipl_build_hobs(&h, buffer, IPL_HOB_CAPACITY, core_base, 0x20000, entry, 0x110000,
                           0x10000));
    h.pci_mmio_range_count = 0;
    h.ram[1].base = 0x700000;
    assert(!ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    h.ram[1].base = 0x900000;
    h.fw_data_base = 0x7f0000; /* Crosses the 8..9 MiB hole. */
    assert(!ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    h.ram_range_count = IA64VMM_HANDOFF_MAX_RAM + 1;
    assert(!ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    h.ram_range_count = 3;
    h.ram[1] = (struct ia64vmm_ram_range){0x900000, 0x1700000};
    h.ram[2] = (struct ia64vmm_ram_range){0x2000000, 0x5900000};
    h.fw_data_base = 0x1000000; /* Valid span across adjacent RAM descriptors. */
    assert(ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
    for (unsigned model = 0; model < 2; ++model) {
        ia64vmm_handoff_cpu_model(&h, model);
        assert(ipl_validate_handoff(&h, sizeof(h), IA64VMM_PLATFORM_ID));
        struct ia64vmm_handoff bad = h;
        bad.cpu_report.version++;
        assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
        bad = h;
        bad.cpu_report.size--;
        assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
        bad = h;
        bad.cpu_report.cpuid3 ^= 1;
        assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
        bad = h;
        bad.cpu_report.model = 2;
        assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
        bad = h;
        bad.size = IA64VMM_HANDOFF_BASE_SIZE;
        memset(&bad.cpu_report, 0xff, sizeof(bad.cpu_report));
        assert(ipl_validate_handoff(&bad, bad.size, IA64VMM_PLATFORM_ID));
        struct ia64vmm_handoff copy;
        ia64vmm_handoff_copy(&copy, &bad);
        assert(copy.cpu_report.model == IA64_CPU_MONTECITO && ia64vmm_handoff_cpu_valid(&copy));
        bad = h;
        bad.size = IA64VMM_HANDOFF_BASE_SIZE - 1;
        assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
        bad = h;
        bad.size--;
        assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
        assert(!ipl_validate_handoff(&h, sizeof(h) - 1, IA64VMM_PLATFORM_ID));
        struct {
            struct ia64vmm_handoff h;
            UINT64 future;
        } longer = {h, 0};
        longer.h.size = sizeof(longer);
        longer.h.cpu_report.size += sizeof(longer.future);
        assert(ipl_validate_handoff(&longer.h, sizeof(longer), IA64VMM_PLATFORM_ID));
        ia64vmm_handoff_copy(&copy, &longer.h);
        assert(ipl_validate_handoff(&copy, sizeof(copy), IA64VMM_PLATFORM_ID));
        bad = h;
        memset(bad.cpu_report.brand, 'x', sizeof(bad.cpu_report.brand));
        assert(!ipl_validate_handoff(&bad, sizeof(bad), IA64VMM_PLATFORM_ID));
    }
    test_legacy(&h, core_base, entry);
    test_partial_windows(&h, core_base, entry);
    test_ram_granules(&h, core_base, entry);
    test_granules(&h, core_base, entry);
    return 0;
}
