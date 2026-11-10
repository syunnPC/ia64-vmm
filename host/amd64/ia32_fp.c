/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/ia32_svm.h"
#include "host/amd64/simd.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"

/* First four words and the FSAVE slot are addressed by ia32_fp_switch.S. */
struct amd64_ia32_fp_owner {
    void *host, *initial;
    uint64_t xcr0, host_flags;
    struct amd64_ia32_fsave legacy;
    size_t pages;
    unsigned cpu;
};
_Static_assert(offsetof(struct amd64_ia32_fp_owner, legacy) == 32, "native FP assembly layout");
_Static_assert(sizeof(struct amd64_ia32_fp_owner) <= 4096, "native FP owner page");
bool amd64_ia32_fp_create(struct amd64_ia32_fp_owner **out, struct ia32_setup_error *error)
{
    struct amd64_ia32_fp_requirements r;
    *out = NULL;
    if (!amd64_ia32_fp_requirements(&r, error))
        return false;
    if (r.host_x87_initial_only && !r.xgetbv1) {
        if (error)
            *error = (struct ia32_setup_error){
                .reason = IA32_EXIT_UNIMPLEMENTED,
                .host_cpu = hal_cpu_index(),
                .detail = "native FP needs a host x87 initial-state indication"};
        return false;
    }
    struct amd64_ia32_fp_owner *o = hal_alloc_pages(1, HAL_PAGE_ZERO);
    if (!o)
        goto fail;
    o->pages = (r.bytes + 4095) / 4096;
    o->cpu = hal_cpu_index();
    o->xcr0 = r.xcr0;
    o->host_flags = r.host_x87_initial_only ? 1 : 0;
    o->host = hal_alloc_pages(o->pages, HAL_PAGE_ZERO);
    o->initial = hal_alloc_pages(o->pages, HAL_PAGE_ZERO);
    if (!o->host || !o->initial) {
        amd64_ia32_fp_destroy(o);
        goto fail;
    }
    if (!amd64_xstate_init(o->initial, r.bytes)) {
        amd64_ia32_fp_destroy(o);
        goto fail;
    }
    *out = o;
    return true;
fail:
    if (error)
        *error = (struct ia32_setup_error){.reason = IA32_EXIT_ENTRY_FAILURE,
                                           .host_cpu = hal_cpu_index(),
                                           .detail = "native FP state allocation failed"};
    return false;
}
void amd64_ia32_fp_destroy(struct amd64_ia32_fp_owner *o)
{
    if (!o || o->cpu != hal_cpu_index())
        return;
    if (o->host)
        hal_free_pages(o->host, o->pages);
    if (o->initial)
        hal_free_pages(o->initial, o->pages);
    hal_free_pages(o, 1);
}
bool amd64_ia32_fp_prepare(struct amd64_ia32_fp_owner *o, const struct ia32_fp_image *fp,
                           struct ia32_setup_error *error)
{
    uint32_t mxcsr = (uint32_t)fp->bytes[24] | (uint32_t)fp->bytes[25] << 8 |
                     (uint32_t)fp->bytes[26] << 16 | (uint32_t)fp->bytes[27] << 24;
    if (!o || o->cpu != hal_cpu_index() || (mxcsr & ~UINT32_C(0xffbf))) {
        if (error)
            *error =
                (struct ia32_setup_error){.reason = IA32_EXIT_UNIMPLEMENTED,
                                          .host_cpu = hal_cpu_index(),
                                          .detail = "native FP owner or legacy MXCSR is invalid"};
        return false;
    }
    amd64_ia32_fp_pack(fp, &o->legacy);
    uint8_t *initial = o->initial;
    memcpy(initial + 24, fp->bytes + 24, 4);
    memcpy(initial + 160, fp->bytes + 160, 128);
    /* Restore SSE but initialize x87 and every enabled upper component. */
    initial[512] = 2;
    return true;
}
bool amd64_ia32_fp_host_admit(struct amd64_ia32_fp_owner *o, struct ia32_setup_error *error)
{
    if (o->host_flags && (amd64_ia32_fp_host_inuse() & 1)) {
        if (error)
            *error = (struct ia32_setup_error){
                .reason = IA32_EXIT_UNIMPLEMENTED,
                .host_cpu = hal_cpu_index(),
                .detail = "native FP cannot preserve noninitial AMD/Hygon host x87 state"};
        return false;
    }
    return true;
}
void amd64_ia32_fp_complete(struct amd64_ia32_fp_owner *o, struct ia32_fp_image *fp)
{
    amd64_ia32_fp_unpack(&o->legacy, fp);
}
