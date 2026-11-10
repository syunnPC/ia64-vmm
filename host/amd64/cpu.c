/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/amd64.h"
#include "host/amd64/boot_diag.h"
#include "host/amd64/simd.h"
#include "host/amd64/smp.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "host/fb/fb_console.h"
#include "core/log_internal.h"
#include "engine/hostwin/hostwin.h"

struct descriptor_pointer {
    uint16_t limit;
    uint64_t base;
} IA64VMM_PACKED;
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3], reserved1, ist[7], reserved2;
    uint16_t reserved3, iomap;
} IA64VMM_PACKED;
struct idt_gate {
    uint16_t offset0, selector;
    uint8_t ist, type;
    uint16_t offset1;
    uint32_t offset2, reserved;
} IA64VMM_PACKED;
static uint64_t early_gdt[5];
static struct tss64 early_tss;
static uint8_t early_ist[3][16384] IA64VMM_ALIGNED(4096);
static struct idt_gate early_idt[256] IA64VMM_ALIGNED(16);
static struct idt_gate *idt = early_idt;
extern void *amd64_exception_stubs[32];
extern void *amd64_interrupt_stubs[224];
extern void amd64_load_gdt(const struct descriptor_pointer *ptr);

/* Shared only with memory.c: PAT[7] is reserved for framebuffer mappings. */
bool amd64_fb_wc;
static void framebuffer_pat_init(bool initial)
{
    uint32_t a, b, c, d;
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    bool available = (d & (1u << 16)) != 0;
    /* Only the early BSP publishes the mapping policy. APs must install
     * that policy without racing writes to shared boot capability state. */
    if (initial)
        amd64_fb_wc = available;
    else if (available != amd64_fb_wc)
        hal_panic("inconsistent PAT capability across CPUs");
    if (!available) {
        LOGW("PAT unavailable: framebuffer falling back to UC-");
        return;
    }
    /* Limine specifies PAT[0..5], including the architectural defaults in
     * [0..3]. Preserve them and put WC (type 1) in unused entry 7. Flush
     * caches and all TLB entries around the change (SDM Vol.3A 12.12.4).
     * Every AP installs the identical PAT before using our shared page tables. */
    uint64_t pat = (amd64_rdmsr(0x277) & UINT64_C(0x00ffffffffffffff)) | (UINT64_C(1) << 56);
    uint64_t cr0, cr3, cr4;
    __asm__ volatile("mov %%cr0,%0; mov %%cr3,%1; mov %%cr4,%2" : "=r"(cr0), "=r"(cr3), "=r"(cr4));
    uint64_t uncached = (cr0 | (UINT64_C(1) << 30)) & ~(UINT64_C(1) << 29);
    uint64_t nonglobal = cr4 & ~(UINT64_C(1) << 7);
    __asm__ volatile("mov %0,%%cr0; wbinvd; mov %1,%%cr4; mov %2,%%cr3" ::"r"(uncached),
                     "r"(nonglobal), "r"(cr3)
                     : "memory");
    amd64_wrmsr(0x277, pat);
    __asm__ volatile("wbinvd; mov %0,%%cr3; mov %1,%%cr4; mov %2,%%cr0" ::"r"(cr3), "r"(cr4),
                     "r"(cr0)
                     : "memory");
    LOGI("PAT=%016llx: framebuffer WC at PAT[7]", (unsigned long long)pat);
}

static void cpu_features(bool initial)
{
    uint32_t a, b, c, d;
    amd64_cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000001)
        hal_panic("CPU lacks extended feature leaf / NX");
    amd64_cpuid(0x80000001, 0, &a, &b, &c, &d);
    if (!(d & (1u << 20)))
        hal_panic("CPU lacks NX: cannot enforce W^X");
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0,%0; mov %%cr4,%1" : "=r"(cr0), "=r"(cr4));
    /* WP; PGE for the global upper-half leaves. */
    cr0 |= 1u << 16;
    cr4 |= 1u << 7;
    __asm__ volatile("mov %0,%%cr0; mov %1,%%cr4" ::"r"(cr0), "r"(cr4) : "memory");
    amd64_wrmsr(0xc0000080, amd64_rdmsr(0xc0000080) | (1u << 11));
    framebuffer_pat_init(initial);
}

uint64_t amd64_xcr0;
unsigned amd64_xstate_bytes;
__attribute__((noinline)) unsigned amd64_simd_enable(void)
{
    unsigned missing = amd64_simd_missing(amd64_cpuid);
    if (missing)
        return missing;
    uint32_t a, b, c, d;
    amd64_cpuid(0xd, 0, &a, &b, &c, &d);
    uint64_t supported = (uint64_t)d << 32 | a;
    /* The boot CPU's first call chooses the mask; every other CPU, and the
     * boot CPU again from amd64_smp_online, applies it unchanged. */
    bool first = !amd64_xcr0;
    uint64_t mask = first ? amd64_xcr0_mask(supported) : amd64_xcr0;
    if ((supported & mask) != mask) {
        char message[128];
        vmm_snprintf(message, sizeof(message),
                     "CPU lacks XSAVE state components of XCR0 %llx (supports %llx)",
                     (unsigned long long)mask, (unsigned long long)supported);
        hal_panic(message);
    }
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0,%0; mov %%cr4,%1" : "=r"(cr0), "=r"(cr4));
    /* SDM Vol.3A §16.1.3: MP set, EM and TS clear, CR4.OSFXSR and
     * OSXMMEXCPT. NE selects native x87 error reporting (#MF) as every
     * 64-bit kernel does; VMXON also requires it (Vol.3C §26.8,
     * IA32_VMX_CR0_FIXED0 bit 5). Vol.1 §13.3: CR4.OSXSAVE, then XCR0;
     * a mask taken from CPUID.(0DH,0) keeps every dependent group whole
     * (§14.3: AVX needs XCR0[2:1] = 11b). */
    cr0 = (cr0 | (1u << 5) | (1u << 1)) & ~((1u << 2) | (1u << 3));
    cr4 |= (1u << 9) | (1u << 10) | (1u << 18);
    uint32_t mxcsr = AMD64_MXCSR_DEFAULT;
    __asm__ volatile("mov %0,%%cr0; mov %1,%%cr4; xsetbv; ldmxcsr %4" ::"r"(cr0), "r"(cr4),
                     "c"(0), "a"((uint32_t)mask), "m"(mxcsr), "d"((uint32_t)(mask >> 32))
                     : "memory");
    amd64_xstate_init_x87();
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    amd64_cpuid(1, 0, &a, &b, &c, &d);
    if (((uint64_t)hi << 32 | lo) != mask || !(c & (1u << 27)))
        hal_panic("XCR0 or CR4.OSXSAVE did not take the chosen mask");
    /* Vol.2 CPUID leaf 0DH: EBX is the XSAVE area size for the enabled XCR0.
     * Areas are allocated for the boot CPU's size. */
    amd64_cpuid(0xd, 0, &a, &b, &c, &d);
    if (b < AMD64_XSAVE_MIN_BYTES)
        hal_panic("XSAVE area size below the legacy region and header");
    if (first) {
        amd64_xstate_bytes = b;
        amd64_xcr0 = mask;
    } else if (b > amd64_xstate_bytes)
        hal_panic("XSAVE area larger than the boot CPU's");
    return 0;
}

void amd64_idt_gate(unsigned vector, void *handler)
{
    uint64_t addr = (uintptr_t)handler;
    idt[vector] = (struct idt_gate){
        .offset0 = addr,
        .selector = 8,
        .type = 0x8e,
        .offset1 = addr >> 16,
        .offset2 = addr >> 32,
        .ist = vector == 8    ? 1
               : vector == 2  ? 2
               : vector == 18 ? 3
                              : 0,
    };
}
static void descriptors(uint64_t *gdt, struct tss64 *tss, uint8_t *stacks, void *top)
{
    gdt[1] = UINT64_C(0x00af9a000000ffff);
    gdt[2] = UINT64_C(0x00cf92000000ffff);
    tss->rsp[0] = (uintptr_t)top;
    for (unsigned i = 0; i < 3; ++i)
        tss->ist[i] = (uintptr_t)stacks + (i + 1) * 16384;
    tss->iomap = sizeof(*tss);
    uint64_t base = (uintptr_t)tss, limit = sizeof(*tss) - 1;
    gdt[3] =
        limit | ((base & 0xffffff) << 16) | (UINT64_C(0x89) << 40) | (((base >> 24) & 0xff) << 56);
    gdt[4] = base >> 32;
    struct descriptor_pointer gdtr = {5 * sizeof(*gdt) - 1, (uintptr_t)gdt};
    amd64_load_gdt(&gdtr);
    struct descriptor_pointer idtr = {256 * sizeof(*idt) - 1, (uintptr_t)idt};
    __asm__ volatile("lidt %0" ::"m"(idtr) : "memory");
}
void amd64_cpu_init(void)
{
    cpu_features(true);
    for (unsigned i = 0; i < 256; ++i)
        amd64_idt_gate(i, i < 32 ? amd64_exception_stubs[i] : amd64_interrupt_stubs[i - 32]);
    descriptors(early_gdt, &early_tss, &early_ist[0][0], boot_stack_top);
}
void amd64_cpu_init_index(unsigned index, void *stack_top)
{
    char missing[96];
    unsigned lacks = amd64_simd_enable();
    if (lacks) {
        char message[160];
        vmm_snprintf(message, sizeof(message),
                     "CPU%u lacks host SIMD baseline features the boot CPU has: %s", index,
                     amd64_simd_describe(lacks, missing, sizeof(missing)));
        hal_panic(message);
    }
    cpu_features(false);
    if (index == 0) {
        struct idt_gate *owned = hal_alloc_pages(1, HAL_PAGE_ZERO);
        if (!owned)
            hal_panic("cannot relocate IDT");
        for (unsigned i = 0; i < 256; ++i)
            owned[i] = idt[i];
        idt = owned;
    }
    uint8_t *owned = hal_alloc_pages(13, HAL_PAGE_ZERO);
    if (!owned)
        hal_panic("cannot allocate per-CPU GDT/TSS/IST stacks");
    descriptors((uint64_t *)owned, (struct tss64 *)(owned + 64), owned + 4096, stack_top);
}

/* This layout follows the pushes in exceptions.S and the IA-32e hardware
 * exception frame (Intel SDM Vol.3, interrupt/exception handling). */
struct exception_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error, rip, cs, rflags, rsp, ss;
};
void amd64_exception(struct exception_frame *f)
{
    if (f->vector == 2 && amd64_cpus && hal_cpu_index() != 0 &&
        __atomic_load_n(&amd64_current_cpu()->stop, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&amd64_current_cpu()->stopped, 1, __ATOMIC_RELEASE);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    uint64_t cr2;
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    if (amd64_boot_diag_recover(f->vector, f->error, &f->rip, cr2))
        return;
#if !defined(IA64VMM_AMD64_HOST_ONLY)
    if (f->vector == 14 && cr2 < (UINT64_C(1) << 47) && !(f->error & (4u | 8u | 16u)) &&
        hostwin_owns(cr2)) {
        struct hostwin_trap t = {
            .gpr = {f->rax, f->rcx, f->rdx, f->rbx, f->rsp, f->rbp, f->rsi, f->rdi, f->r8, f->r9,
                    f->r10, f->r11, f->r12, f->r13, f->r14, f->r15},
            .rip = f->rip,
            .rflags = f->rflags,
            .fault_va = cr2,
            .write = (f->error & 2) != 0,
        };
        /* The client only redirects the saved RIP. Resolution runs after
         * IRETQ, with the region's ring-0 stack and original IF restored. */
        if (hostwin_trap(&t) && t.gpr[4] == f->rsp) {
            f->rax = t.gpr[0];
            f->rcx = t.gpr[1];
            f->rdx = t.gpr[2];
            f->rbx = t.gpr[3];
            f->rbp = t.gpr[5];
            f->rsi = t.gpr[6];
            f->rdi = t.gpr[7];
            f->r8 = t.gpr[8];
            f->r9 = t.gpr[9];
            f->r10 = t.gpr[10];
            f->r11 = t.gpr[11];
            f->r12 = t.gpr[12];
            f->r13 = t.gpr[13];
            f->r14 = t.gpr[14];
            f->r15 = t.gpr[15];
            f->rip = t.rip;
            f->rflags = t.rflags;
            return;
        }
    }
#endif
    log_panic_begin();
    if (hal_cpu_index() == 0) {
        amd64_smp_stop();
        fb_console_reclaim();
    }
    /* Every line carries the "host exc: " prefix and stays within 79
     * columns with the log's "[info] " (fb_console wraps at the 80th): the
     * exit summary clears the screen and reprints them
     * (host/amd64/checkpoints.c), so a machine without serial and with an
     * 80x30 console (640x480) still shows the vector, the
     * registers and the return addresses on the stack. */
    static const char *const names[] = {"#DE", "#DB",  "NMI", "#BP", "#OF", "#BR", "#UD",
                                        "#NM", "#DF",  "CSO", "#TS", "#NP", "#SS", "#GP",
                                        "#PF", "rsvd", "#MF", "#AC", "#MC", "#XM"};
    LOGE("host exc: vector=%llu %s error=%016llx", (unsigned long long)f->vector,
         f->vector < IA64VMM_ARRAY_SIZE(names) ? names[f->vector] : "",
         (unsigned long long)f->error);
    LOGE("host exc: RIP=%016llx CR2=%016llx", (unsigned long long)f->rip, (unsigned long long)cr2);
    if (f->vector == 14)
        LOGE("host exc: #PF %s %s %s rsvd=%u ifetch=%u pk=%u ss=%u",
             f->error & 1 ? "protection" : "not-present", f->error & 2 ? "write" : "read",
             f->error & 4 ? "user" : "supervisor", (unsigned)((f->error >> 3) & 1),
             (unsigned)((f->error >> 4) & 1), (unsigned)((f->error >> 5) & 1),
             (unsigned)((f->error >> 6) & 1));
    if (f->vector == 6)
        LOGE("host exc: #UD invalid opcode at RIP (or unsupported host ISA)");
    if (f->vector == 13)
        LOGE("host exc: #GP selector/error=%04llx", (unsigned long long)f->error);
    const uint64_t regs[] = {f->rax, f->rbx, f->rcx, f->rdx, f->rsi, f->rdi, f->rbp, f->rsp,
                             f->r8,  f->r9,  f->r10, f->r11, f->r12, f->r13, f->r14, f->r15};
    static const char *const reg_names[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                            "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    for (unsigned i = 0; i < 15; i += 3)
        LOGE("host exc: %s=%016llx %s=%016llx %s=%016llx", reg_names[i],
             (unsigned long long)regs[i], reg_names[i + 1], (unsigned long long)regs[i + 1],
             reg_names[i + 2], (unsigned long long)regs[i + 2]);
    LOGE("host exc: r15=%016llx rfl=%llx cs=%llx ss=%llx CPU%u", (unsigned long long)f->r15,
         (unsigned long long)f->rflags, (unsigned long long)f->cs, (unsigned long long)f->ss,
         amd64_cpus ? hal_cpu_index() : 0);
    /* 12 stack quadwords, three per line: the return addresses name the
     * callers of the faulting function. */
    for (unsigned row = 0; row < 4; ++row) {
        uint64_t q[3];
        unsigned have = 0;
        for (; have < 3; ++have) {
            uint64_t addr = f->rsp + (row * 3 + have) * 8;
            if (addr < f->rsp || !amd64_readable(addr, 8))
                break;
            q[have] = *(const uint64_t *)(uintptr_t)addr;
        }
        if (!have) {
            LOGE("host exc: stack+%02x unavailable", row * 24);
            break;
        }
        char text[64];
        size_t used = 0;
        for (unsigned i = 0; i < have; ++i)
            used += (size_t)vmm_snprintf(text + used, sizeof(text) - used, " %016llx",
                                         (unsigned long long)q[i]);
        LOGE("host exc: stack+%02x%s", row * 24, text);
        if (have < 3)
            break;
    }
    hal_panic("fatal host exception");
}
