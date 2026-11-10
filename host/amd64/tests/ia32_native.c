/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/amd64/ia32_native.h"
#include "host/amd64/ia32_native_mode.h"
#include "tests/host/test.h"

uint64_t amd64_hhdm;
const struct ia32_backend_ops amd64_svm_ia32_ops = {0}, amd64_vmx_ia32_ops = {0};
static unsigned allocations;
unsigned hal_cpu_index(void)
{
    return 0;
}
bool amd64_memory_physical(uint64_t va, uint64_t *pa)
{
    if (va < 4096)
        return false;
    *pa = va - 4096;
    return true;
}
void *hal_alloc_pages(size_t pages, unsigned flags)
{
    (void)flags;
    void *p = aligned_alloc(4096, pages * 4096);
    if (p) {
        memset(p, 0, pages * 4096);
        ++allocations;
    }
    return p;
}
void hal_free_pages(void *p, size_t pages)
{
    (void)pages;
    --allocations;
    free(p);
}
static uint64_t leaf_at(const struct amd64_ia32_map *m, uint64_t token)
{
    const uint64_t *p = m->pages[0];
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        uint64_t entry = p[(token >> shift) & 511];
        if (!entry)
            return 0;
        p = (void *)(uintptr_t)(entry & UINT64_C(0x000ffffffffff000));
    }
    return p[(token >> 12) & 511];
}

int main(void)
{
    struct ia32_native_state mode = {.cpl = 3, .eflag = 0x20202};
    mode.cs =
        (struct ia32_segment){.base = 0x12340, .limit = 0xffff, .selector = 0x1234, .attrib = 0xf3};
    mode.ss = mode.ds = mode.es = mode.fs = mode.gs = mode.cs;
    mode.fp.bytes[12] = 0xfb;
    mode.fp.bytes[13] = 0xff;
    mode.fp.bytes[20] = 0xf3;
    mode.fp.bytes[21] = 0xff;
    struct amd64_ia32_segment_view view = amd64_ia32_segment_view(&mode);
    CHECK((view.cs & 3) == 3 && (view.ss & 3) == 3 && view.cs != view.ss);
    CHECK(view.cs != 0xfffb && view.ss != 0xfff3 && view.cs != mode.cs.selector);
    struct ia32_segment private_cs = amd64_ia32_private_segment(&mode, 1);
    CHECK(private_cs.base == mode.cs.base && private_cs.limit == mode.cs.limit &&
          private_cs.attrib == 0xfb && private_cs.selector == view.cs);
    CHECK(amd64_ia32_private_eflag(&mode) == 0x202);
    CHECK(amd64_ia32_semantic_eflag(&mode, 0x203) == 0x20203);
    mode.fp.bytes[12] = view.cs;
    mode.fp.bytes[13] = view.cs >> 8;
    mode.fp.bytes[20] = view.ss;
    mode.fp.bytes[21] = view.ss >> 8;
    amd64_ia32_restore_fp_selectors(&mode, view);
    CHECK(amd64_ia32_fp_selector(&mode.fp, 12) == mode.cs.selector &&
          amd64_ia32_fp_selector(&mode.fp, 20) == mode.ss.selector);
    struct ia32_native_exit boundary = {.reason = IA32_EXIT_HOST_IRQ, .phase = IA32_PHASE_UNKNOWN};
    amd64_ia32_step_host_phase(&boundary, false);
    CHECK(boundary.phase == IA32_PHASE_UNKNOWN);
    amd64_ia32_step_host_phase(&boundary, true);
    CHECK(boundary.phase == IA32_PHASE_BEFORE);
    boundary.phase = IA32_PHASE_UNKNOWN;
    boundary.vectoring.valid = true;
    amd64_ia32_step_host_phase(&boundary, true);
    CHECK(boundary.phase == IA32_PHASE_UNKNOWN);
    hpa_t backing;
    CHECK(hal_ia32_backing_hpa((void *)(uintptr_t)0x11000, &backing) && backing == 0x10000);
    CHECK(!hal_ia32_backing_hpa((void *)(uintptr_t)1, &backing));
    CHECK(!hal_ia32_backing_hpa(NULL, &backing));
    struct ia32_mapping m = {.token = {0x1000},
                             .hpa = 0x12345000,
                             .rights = IA32_MAP_READ | IA32_MAP_EXEC,
                             .memory_type = IA32_MEMORY_WB};
    uint64_t leaf;
    /* SDM EPT leaf: R/W/X 0:2, memory type 3:5, ignore PAT 6. */
    CHECK(amd64_ia32_map_leaf(IA32_BACKEND_VMX, false, 48, &m, &leaf));
    CHECK(leaf == (m.hpa | 0x75));
    m.rights = IA32_MAP_EXEC;
    CHECK(!amd64_ia32_map_leaf(IA32_BACKEND_VMX, false, 48, &m, &leaf));
    CHECK(amd64_ia32_map_leaf(IA32_BACKEND_VMX, true, 48, &m, &leaf));
    CHECK(leaf == (m.hpa | 0x74));
    CHECK(!amd64_ia32_map_leaf(IA32_BACKEND_SVM, true, 48, &m, &leaf));
    m.rights = IA32_MAP_WRITE;
    CHECK(!amd64_ia32_map_leaf(IA32_BACKEND_VMX, true, 48, &m, &leaf));
    CHECK(!amd64_ia32_map_leaf(IA32_BACKEND_SVM, true, 48, &m, &leaf));
    /* APM 15.25: 4 KiB NPT P/RW/US, NX, and PAT[3] UC. */
    m.rights = IA32_MAP_READ | IA32_MAP_WRITE;
    m.memory_type = IA32_MEMORY_UC;
    CHECK(amd64_ia32_map_leaf(IA32_BACKEND_SVM, false, 48, &m, &leaf));
    CHECK(leaf == (m.hpa | UINT64_C(0x800000000000001f)));
    m.memory_type = IA32_MEMORY_WC;
    CHECK(amd64_ia32_map_leaf(IA32_BACKEND_SVM, false, 48, &m, &leaf));
    CHECK(leaf == (m.hpa | UINT64_C(0x800000000000009f)));
    m.hpa = UINT64_C(1) << 48;
    CHECK(!amd64_ia32_map_leaf(IA32_BACKEND_SVM, false, 48, &m, &leaf));
    m.hpa = 0x1001;
    CHECK(!amd64_ia32_map_leaf(IA32_BACKEND_VMX, true, 48, &m, &leaf));
    m.hpa = 0x1000;
    m.token.value = UINT64_C(1) << 48;
    CHECK(!amd64_ia32_map_leaf(IA32_BACKEND_VMX, true, 48, &m, &leaf));
    m.token.value = 0;
    m.rights = 0;
    CHECK(amd64_ia32_map_leaf(IA32_BACKEND_VMX, true, 48, &m, &leaf) && !leaf);
    struct amd64_ia32_map map;
    struct ia32_setup_error error;
    CHECK(amd64_ia32_map_init(&map, IA32_BACKEND_VMX, true, 52, &error));
    m.rights = IA32_MAP_READ;
    m.memory_type = IA32_MEMORY_WB;
    CHECK(amd64_ia32_map_set(&map, &m, &error));
    CHECK(leaf_at(&map, 0) == 0x1071 && !leaf_at(&map, 4096));
    m.token.value = UINT64_C(0xfffffffff000);
    CHECK(amd64_ia32_map_set(&map, &m, &error));
    CHECK(leaf_at(&map, m.token.value) == 0x1071);
    CHECK(amd64_ia32_map_unset(&map, (ia32_token_t){0}, UINT64_C(1) << 48, &error));
    CHECK(!leaf_at(&map, 0) && !leaf_at(&map, m.token.value));
    CHECK(!amd64_ia32_map_unset(&map, (ia32_token_t){UINT64_C(1) << 48}, 4096, &error));
    amd64_ia32_map_drop(&map);
    CHECK(!allocations);
    puts(
        "native nested maps: exact leaf rights/types, sparse tables and bounded range removal passed");
    return 0;
}
