/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/ia32_native.h"
#include "host/amd64/amd64.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"

static bool fail(struct ia32_setup_error *e, const char *detail)
{
    if (e)
        *e = (struct ia32_setup_error){
            .reason = IA32_EXIT_UNIMPLEMENTED, .host_cpu = hal_cpu_index(), .detail = detail};
    return false;
}
static uint64_t physical(const void *p)
{
    return (uintptr_t)p - amd64_hhdm;
}
static void *table_alloc(struct amd64_ia32_map *m)
{
    if (m->count == IA64VMM_ARRAY_SIZE(m->pages))
        return NULL;
    void *p = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (p)
        m->pages[m->count++] = p;
    return p;
}
bool amd64_ia32_map_init(struct amd64_ia32_map *m, enum ia32_backend_kind backend,
                         bool execute_only, unsigned bits, struct ia32_setup_error *e)
{
    if (!m || bits < 32 || bits > 52 ||
        (backend != IA32_BACKEND_SVM && backend != IA32_BACKEND_VMX))
        return fail(e, "native nested map parameters are invalid");
    *m = (struct amd64_ia32_map){
        .backend = backend, .execute_only = execute_only, .physical_bits = bits};
    void *p = table_alloc(m);
    if (!p)
        return fail(e, "native nested root allocation failed");
    m->root = physical(p);
    return true;
}
static uint64_t *map_slot(struct amd64_ia32_map *m, uint64_t token, bool create)
{
    uint64_t *p = m->pages[0];
    if (!m->count)
        return NULL;
    for (unsigned shift = 39; shift > 12; shift -= 9) {
        uint64_t *slot = &p[(token >> shift) & 511];
        if (!*slot) {
            if (!create)
                return NULL;
            void *next = table_alloc(m);
            if (!next)
                return NULL;
            *slot = physical(next) | 7;
        }
        p = (void *)(uintptr_t)(amd64_hhdm + (*slot & UINT64_C(0x000ffffffffff000)));
    }
    return &p[(token >> 12) & 511];
}
bool amd64_ia32_map_set(struct amd64_ia32_map *m, const struct ia32_mapping *mapping,
                        struct ia32_setup_error *e)
{
    uint64_t entry;
    if (!amd64_ia32_map_leaf(m->backend, m->execute_only, m->physical_bits, mapping, &entry))
        return fail(e, "native nested mapping is unrepresentable");
    /* Native NPT callers use their probed host PAT encoder. This common
     * mapper currently serves EPT, whose memory type is explicit. */
    if (m->backend != IA32_BACKEND_VMX)
        return fail(e, "native NPT mappings require probed host PAT attributes");
    uint64_t *slot = map_slot(m, mapping->token.value, entry != 0);
    if (!slot)
        return entry ? fail(e, "native nested table allocation failed") : true;
    *slot = entry;
    return true;
}
bool amd64_ia32_map_unset(struct amd64_ia32_map *m, ia32_token_t token, uint64_t bytes,
                          struct ia32_setup_error *e)
{
    if (((token.value | bytes) & 4095) || token.value > (UINT64_C(1) << 48) ||
        bytes > (UINT64_C(1) << 48) - token.value)
        return fail(e, "native nested unmap range is invalid");
    if (m->count && bytes)
        amd64_ia32_clear_tables(m->pages[0], 4, 0, token.value, token.value + bytes, amd64_hhdm);
    return true;
}
void amd64_ia32_map_drop(struct amd64_ia32_map *m)
{
    for (unsigned i = 0; i < m->count; ++i)
        hal_free_pages(m->pages[i], 1);
    *m = (struct amd64_ia32_map){0};
}
const struct ia32_backend_ops *hal_ia32_backend(void)
{
    uint32_t a, b, c, d;
    amd64_cpuid(0, 0, &a, &b, &c, &d);
    if (b == UINT32_C(0x68747541) && d == UINT32_C(0x69746e65) && c == UINT32_C(0x444d4163))
        return &amd64_svm_ia32_ops;
    if (b == UINT32_C(0x6f677948) && d == UINT32_C(0x6e65476e) && c == UINT32_C(0x656e6975))
        return &amd64_svm_ia32_ops;
    if (b == UINT32_C(0x756e6547) && d == UINT32_C(0x49656e69) && c == UINT32_C(0x6c65746e))
        return &amd64_vmx_ia32_ops;
    return NULL;
}
bool hal_ia32_backing_hpa(const void *backing, hpa_t *out)
{
    return backing && out && amd64_memory_physical((uintptr_t)backing, out);
}
