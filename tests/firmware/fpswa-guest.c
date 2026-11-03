/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Uefi.h>
#include <stdbool.h>
#include "firmware/new/fpswa/fpswa.h"
#include "firmware/new/ivt-layout.h"
#include "ia64vmm/platform.h"
#include "fpswa-vectors.h"

static EFI_SYSTEM_TABLE *system;
static struct fpswa_interface *fpswa;
static uint64_t control[1024] __attribute__((aligned(16)));
static uint8_t interrupt_stack[65536] __attribute__((aligned(16)));
static volatile uint64_t calls, failure, last_status;
static uint64_t last_error, last_raw;
static uint64_t live_dest;
static bool at_runtime, translated, owns_ivt, native_bridge;
static uint64_t firmware_iva, firmware_k1, firmware_psr;
extern unsigned char ivt_base[];
extern void fpswa_guest_execute(uint64_t, uint64_t, const struct fpswa_fr *,
                                const struct fpswa_fr *, void *);
extern void gate_enable_translation(void);
static void own_ivt(void)
{
    __asm__ volatile(
        "rsm psr.i;; srlz.d;; mov ar.k1=%0; mov cr.iva=%1;; srlz.i;; ssm psr.ic;; srlz.d;;" ::"r"(
            control),
        "r"(ivt_base)
        : "memory");
    owns_ivt = true;
}
static void firmware_ivt(void)
{
    __asm__ volatile(
        "rsm psr.i;; srlz.d;; mov ar.k1=%0; mov cr.iva=%1;; srlz.i;; mov psr.l=%2;; srlz.d;;" ::"r"(
            firmware_k1),
        "r"(firmware_iva), "r"(firmware_psr)
        : "memory");
    owns_ivt = false;
}
static void text(const char *s)
{
    if (at_runtime) {
        uint64_t base = IA64VMM_SERVICE_BASE + (translated ? UINT64_C(0xe000000000000000) : 0);
        while (*s)
            *(volatile uint64_t *)(base + IA64VMM_SVC_CONSOLE) = (uint8_t)*s++;
        return;
    }
    CHAR16 out[128];
    unsigned k = 0;
    do {
        out[k++] = (UINT8)*s;
    } while (*s++ && k < 128);
    out[k - 1] = 0;
    bool restore = owns_ivt;
    if (restore)
        firmware_ivt();
    system->ConOut->OutputString(system->ConOut, out);
    if (restore)
        own_ivt();
}
static void check(bool value, const char *reason)
{
    if (value)
        return;
    text("FPSWA probe FAIL: ");
    text(reason);
    text("\r\n");
    system->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_ABORTED, 0, NULL);
    for (;;)
        __asm__ volatile("hint @pause");
}
static void hex(uint64_t value)
{
    char s[19] = {'0', 'x'};
    for (unsigned k = 0; k < 16; ++k)
        s[k + 2] = "0123456789abcdef"[(value >> (60 - 4 * k)) & 15];
    s[18] = 0;
    text(s);
}

/* The IVT save/restore is reused from firmware/new/ivt.S. Only the six FRs
 * Linux uses are passed to FPSWA in memory. The copied handler's additional
 * high-FR save needs the live destination copied back before its restore. */
static void dispatch(uint64_t vector, void *ctx)
{
    (void)vector;
    (void)ctx;
    /* The IVT enters C with DT clear. Match its saved IPSR.dt condition,
     * including before SetVirtualAddressMap; region zero maps the context. */
    uint64_t ipsr = control[IPF_CONTEXT_CrIpsr / 8], isr = control[IPF_CONTEXT_CrIsr / 8];
    bool data_translated = (ipsr & (UINT64_C(1) << 17)) != 0;
    if (data_translated)
        __asm__ volatile("ssm psr.dt;; srlz.d;;" ::: "memory");
    uint64_t bundle[2];
    const uint64_t *ip = (void *)(UINTN)control[IPF_CONTEXT_CrIip / 8];
    bundle[0] = ip[0];
    bundle[1] = ip[1];
    struct fpswa_state state = {0xfc0, 0, NULL, (void *)&control[IPF_CONTEXT_F6 / 8], NULL, NULL};
    struct fpswa_ret ret =
        fpswa->fpswa(1, bundle, &ipsr, &control[IPF_CONTEXT_ArFpsr / 8], &isr,
                     &control[IPF_CONTEXT_Pr / 8], &control[IPF_CONTEXT_CrIfs / 8], &state);
    ++calls;
    last_status = ret.status;
    last_error = ret.err1;
    last_raw = bundle[0];
    if (ret.status != 0 && ret.status != 3 && ret.status != 7)
        failure = ret.status;
    if (live_dest == 32) {
        void *p = (void *)&control[4096 / 8];
        __asm__ volatile("stf.spill [%0]=f32;;" ::"r"(p) : "memory");
    }
    unsigned slot = (ipsr >> 41) & 3;
    if (slot == 2) {
        slot = 0;
        control[IPF_CONTEXT_CrIip / 8] += 16;
    } else
        ++slot;
    control[IPF_CONTEXT_CrIpsr / 8] = (ipsr & ~(UINT64_C(3) << 41)) | ((uint64_t)slot << 41);
    if (data_translated)
        __asm__ volatile("rsm psr.dt;; srlz.d;;" ::: "memory");
}
static void vectors(void)
{
    for (unsigned k = 0; k < sizeof(fpswa_guest_vectors) / sizeof(fpswa_guest_vectors[0]); ++k) {
        const struct fpswa_guest_vector *v = &fpswa_guest_vectors[k];
        /* The oracle implements packed pseudo-SWA by clearing p2, without
         * invoking FPSWA. The VMM explicitly requests an assist instead.
         * Scalar requests are the common native FPSWA comparison surface. */
        if (native_bridge && (v->op == 2 || v->op == 3))
            continue;
        struct {
            struct fpswa_fr result;
            uint64_t fpsr, pr;
        } out;
        uint64_t before = calls;
        live_dest = v->op == 4 ? 32 : 0;
        fpswa_guest_execute(v->op, v->fpsr, &v->a, &v->b, &out);
        if (failure) {
            text("FPSWA return ");
            hex(last_status);
            text(" error ");
            hex(last_error);
            text(" bundle ");
            hex(last_raw);
            text("\r\n");
        }
        check(!failure, "IVT FPSWA return");
        check(calls == before + 1, "instruction raised SWA exactly once");
        check(calls == before || last_status == v->status, "exception conversion");
        if (out.result.significand != v->result.significand ||
            out.result.sign_exponent != v->result.sign_exponent) {
            text("FPSWA vector ");
            hex(k);
            text(" got ");
            hex(out.result.significand);
            text(":");
            hex(out.result.sign_exponent);
            text(" expected ");
            hex(v->result.significand);
            text(":");
            hex(v->result.sign_exponent);
            text("\r\n");
        }
        check(out.result.significand == v->result.significand &&
                  out.result.sign_exponent == v->result.sign_exponent,
              "FR result");
        check(out.fpsr == v->result_fpsr, "FPSR flags and controls");
        check(((out.pr >> 6) & 1) == v->predicate, "predicate");
    }
    text(native_bridge ? "FPSWA probe: IVT scalar results, flags and predicates passed\r\n"
                       : "FPSWA probe: IVT scalar/packed results, flags and predicates passed\r\n");
}
static void saved_call(void)
{
    const struct fpswa_guest_vector *v = &fpswa_guest_vectors[0];
    struct fpswa_fr low[10] __attribute__((aligned(16))) = {{0}};
    low[1] = v->a;
    low[2] = v->b;
    struct fpswa_state state = {0xfc0, 0, NULL, low, NULL, NULL};
    uint64_t ipsr = UINT64_C(1) << 41, fpsr = v->fpsr, isr = 8, preds = 1, ifs = 0, psr, after;
    /* Saved operands allow the OS to enter with both FPU partitions disabled.
     * The wrapper must restore those controls and avoid a nested exception. */
    __asm__ volatile("mov %0=psr;; ssm psr.dfl | psr.dfh;; srlz.d;;" : "=r"(psr)::"memory");
    struct fpswa_ret ret =
        fpswa->fpswa(1, (void *)fpswa_guest_bundle, &ipsr, &fpsr, &isr, &preds, &ifs, &state);
    __asm__ volatile("mov %0=psr;; mov psr.l=%1;; srlz.d;;" : "=&r"(after) : "r"(psr) : "memory");
    check(ret.status == v->status && low[0].significand == v->result.significand &&
              low[0].sign_exponent == v->result.sign_exponent,
          "saved-state call with disabled FPU");
    check((after & UINT64_C(0xc0000)) == UINT64_C(0xc0000), "live FPU disable controls preserved");
    check(fpsr == v->result_fpsr && !((preds >> 6) & 1), "saved-state call flags/predicate");
    text("FPSWA probe: saved-state call and live FPU controls passed\r\n");
}
static void virtual_calls(EFI_HANDLE image)
{
    /* DXE Boot Services may enable interrupts and use their own ar.k1
     * context. Restore that pair until ExitBootServices has completed. */
    firmware_ivt();
    EFI_BOOT_SERVICES *bs = system->BootServices;
    UINTN bytes = 0, key, stride;
    UINT32 version;
    check(bs->GetMemoryMap(&bytes, NULL, &key, &stride, &version) == EFI_BUFFER_TOO_SMALL,
          "runtime map size");
    bytes += 16 * stride;
    UINTN capacity = bytes;
    VOID *map;
    check(bs->AllocatePool(EfiLoaderData, bytes, &map) == EFI_SUCCESS, "runtime map allocation");
    check(bs->GetMemoryMap(&bytes, map, &key, &stride, &version) == EFI_SUCCESS, "runtime map");
    const uint64_t *descriptor = (const void *)fpswa->fpswa;
    uint64_t addresses[4] = {(uint64_t)(UINTN)fpswa, (uint64_t)(UINTN)descriptor, descriptor[0],
                             descriptor[1]};
    for (unsigned k = 0; k < 4; ++k) {
        bool found = false;
        for (UINTN at = 0; at < bytes; at += stride) {
            EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)map + at);
            if (addresses[k] >= d->PhysicalStart &&
                addresses[k] - d->PhysicalStart < d->NumberOfPages * 4096) {
                found = (d->Attribute & EFI_MEMORY_RUNTIME) &&
                        (d->Type == EfiRuntimeServicesCode || d->Type == EfiRuntimeServicesData);
                break;
            }
        }
        check(found, "FPSWA protocol/descriptor/entry/GP runtime memory");
    }
    text("FPSWA probe: runtime memory types passed\r\n");
    bytes = capacity;
    check(bs->GetMemoryMap(&bytes, map, &key, &stride, &version) == EFI_SUCCESS,
          "final runtime map");
    check(bs->ExitBootServices(image, key) == EFI_SUCCESS, "ExitBootServices");
    at_runtime = true;
    own_ivt();
    for (UINTN at = 0; at < bytes; at += stride) {
        EFI_MEMORY_DESCRIPTOR *d = (VOID *)((UINT8 *)map + at);
        d->VirtualStart = d->PhysicalStart +
                          ((d->Attribute & EFI_MEMORY_RUNTIME) ? UINT64_C(0xe000000000000000) : 0);
    }
    uint64_t region = UINT64_C(0xe000000000000000), rr = (7 << 8) | (12 << 2), itir = 32 << 2,
             pte = 0x661, tr = 0;
    __asm__ volatile("rsm psr.ic;; srlz.d;; mov rr[%0]=%1;; srlz.d;;" ::"r"(region), "r"(rr)
                     : "memory");
    __asm__ volatile(
        "mov cr.ifa=%0; mov cr.itir=%1;; itr.i itr[%2]=%3;; srlz.i;; itr.d dtr[%2]=%3;; srlz.d;;" ::
            "r"(region),
        "r"(itir), "r"(tr), "r"(pte)
        : "memory");
    region = 0;
    tr = 1;
    __asm__ volatile(
        "mov cr.ifa=%0; mov cr.itir=%1;; itr.i itr[%2]=%3;; srlz.i;; itr.d dtr[%2]=%3;; srlz.d;;" ::
            "r"(region),
        "r"(itir), "r"(tr), "r"(pte)
        : "memory");
    region = UINT64_C(0xe000000000000000) + (IA64VMM_SERVICE_BASE & ~UINT64_C(0xffffff));
    itir = 24 << 2;
    pte = (IA64VMM_SERVICE_BASE & ~UINT64_C(0xffffff)) | 0x671;
    tr = 2;
    __asm__ volatile(
        "mov cr.ifa=%0; mov cr.itir=%1;; itr.d dtr[%2]=%3;; srlz.d;; ssm psr.ic;; srlz.d;;" ::"r"(
            region),
        "r"(itir), "r"(tr), "r"(pte)
        : "memory");
    EFI_STATUS status = system->RuntimeServices->SetVirtualAddressMap(bytes, stride, version, map);
    if (EFI_ERROR(status)) {
        text("FPSWA virtual status ");
        hex(status);
        text("\r\n");
    }
    check(status == EFI_SUCCESS, "SetVirtualAddressMap");
    check((uint64_t)(UINTN)fpswa->fpswa == addresses[1] + UINT64_C(0xe000000000000000),
          "converted FPSWA descriptor pointer");
    gate_enable_translation();
    translated = true;
    vectors();
    text("FPSWA probe: ExitBootServices/nonidentity virtual calls passed\r\n");
    system->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
}
EFI_STATUS EFIAPI _ModuleEntryPoint(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    system = st;
    EFI_GUID guid = FPSWA_GUID;
    EFI_HANDLE handle;
    UINTN bytes = sizeof(handle);
    check(st->BootServices->LocateHandle(ByProtocol, &guid, NULL, &bytes, &handle) == EFI_SUCCESS,
          "LocateHandle");
    check(st->BootServices->HandleProtocol(handle, &guid, (VOID **)&fpswa) == EFI_SUCCESS,
          "HandleProtocol");
    check(fpswa->revision >= FPSWA_REVISION && fpswa->reserved == 0 && fpswa->fpswa,
          "FPSWA_INTERFACE");
    const uint64_t *entry = (const void *)(UINTN)((const uint64_t *)fpswa->fpswa)[0];
    native_bridge = ((entry[0] >> 5) & UINT64_C(0x1ffffffffff)) == UINT64_C(0x1000000040);
    text("FPSWA probe: protocol located\r\n");
    if (!native_bridge)
        saved_call();
    __asm__ volatile("mov %0=cr.iva; mov %1=ar.k1; mov %2=psr;;"
                     : "=r"(firmware_iva), "=r"(firmware_k1), "=r"(firmware_psr));
    const uint64_t *descriptor = (const void *)dispatch;
    control[5688 / 8] = descriptor[0];
    control[5696 / 8] = descriptor[1];
    /* The reused IVT calls C with DT clear on its own physical stack when
     * the interrupted context has DT set (SDM Vol.2 §5.5). */
    control[CONTROL_InterruptStack / 8] =
        (uint64_t)(UINTN)(interrupt_stack + sizeof(interrupt_stack) - 16);
    own_ivt();
    vectors();
    /* Native fork oracle has a physical break bridge rather than this
     * runtime C implementation. Compare the same executed vectors there;
     * the nonidentity test belongs to the two implementations under test. */
    if (native_bridge) {
        firmware_ivt();
        text("FPSWA probe: native fork scalar differential passed\r\n");
        text("FPSWA probe: native packed pseudo-SWA does not invoke FPSWA\r\n");
        st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }
    virtual_calls(image);
    firmware_ivt();
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}
