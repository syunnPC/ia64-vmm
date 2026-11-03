/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AP rendezvous host checks: the
 * OS_BOOT_RENDEZ registration and the area publication in their store order,
 * the per-CPU area layout for 1..256 CPUs, and SAL's ap_wake_block pointer after
 * SetVirtualAddressMap, written through a second mapping of the same pages. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>
struct store {
    const volatile void *at;
    uint64_t value;
    int release;
};
static struct store stores[8];
static unsigned stored;
#define AP_STORE(lvalue, v)                                                                        \
    (stores[stored++] = (struct store){&(lvalue), (uint64_t)(v), 0}, (lvalue) = (v))
#define AP_RELEASE(lvalue, v)                                                                      \
    (stores[stored++] = (struct store){&(lvalue), (uint64_t)(v), 1},                               \
     __atomic_store_n(&(lvalue), (v), __ATOMIC_RELEASE))
#undef NULL
#include "firmware/new/sal/runtime.c"
#include "firmware/new/sal/sal.c"
#include "firmware/new/ivt-layout.h"

static uint8_t *physical, *alias;
static size_t mapped;
static BOOLEAN convert_fails;
static EFI_STATUS EFIAPI convert(UINTN type, VOID **address)
{
    assert(type == 0);
    uint8_t *p = *address;
    if (convert_fails || p < physical || p >= physical + mapped)
        return EFI_NOT_FOUND;
    *address = alias + (p - physical);
    return EFI_SUCCESS;
}
static EFI_RUNTIME_SERVICES runtimes = {.ConvertPointer = convert};
EFI_RUNTIME_SERVICES *gRT = &runtimes;

static struct sal_call_result rendez(struct sal_state *s, uint64_t entry, uint64_t gp,
                                     uint64_t length)
{
    stored = 0;
    return sal_dispatch(s, 0x01000000, 2, entry, gp, length, 0, 0, 0);
}
static void registered(const volatile struct ap_wake_block *ap, uint64_t entry, uint64_t gp)
{
    assert(ap->rendez_entry == entry && ap->rendez_gp == gp);
    /* gp first, then the entry an AP acquire-loads (ap.S). */
    assert(stored == 2 && stores[0].at == &ap->rendez_gp && stores[0].value == gp &&
           !stores[0].release && stores[1].at == &ap->rendez_entry && stores[1].value == entry &&
           stores[1].release);
}
static void layout(unsigned count)
{
    const uint64_t base = UINT64_C(0x1c848000);
    struct span {
        uint64_t start, end;
    } parts[4 * 256];
    unsigned n = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t area = base + (uint64_t)i * PERCPU_STRIDE;
        parts[n++] =
            (struct span){area + PERCPU_CONTEXT, area + PERCPU_CONTEXT + PERCPU_CONTEXT_SIZE};
        parts[n++] = (struct span){area + PERCPU_RSE, area + PERCPU_RSE + PERCPU_RSE_SIZE};
        parts[n++] = (struct span){area + PERCPU_STACK, area + PERCPU_STACK + PERCPU_STACK_SIZE};
        parts[n++] = (struct span){area + PERCPU_INTERRUPT_STACK,
                                   area + PERCPU_INTERRUPT_STACK + PERCPU_INTERRUPT_STACK_SIZE};
        /* stf.spill needs 16-byte slots; r12 = top - 16 stays in the stack. */
        assert((area + PERCPU_CONTEXT) % 16 == 0 && (area + PERCPU_RSE) % 8 == 0 &&
               (area + PERCPU_STACK + PERCPU_STACK_SIZE - 16) % 16 == 0 &&
               (area + PERCPU_INTERRUPT_STACK + PERCPU_INTERRUPT_STACK_SIZE - 16) % 16 == 0);
    }
    /* Parts are emitted in address order: disjoint and covering exactly the
     * allocation (count * stride, acpi/driver.c), nothing outside it. */
    assert(parts[0].start == base && parts[n - 1].end == base + (uint64_t)count * PERCPU_STRIDE);
    for (unsigned i = 1; i < n; ++i)
        assert(parts[i - 1].end == parts[i].start);
}

int main(void)
{
    /* Area sizes; 8 KiB granular (UEFI 2.7 §7.2.3); the
     * context block holds the whole ivt-layout.h layout. */
    assert(PERCPU_CONTEXT_SIZE == 8192 && PERCPU_RSE_SIZE == 32768 && PERCPU_STACK_SIZE == 65536 &&
           PERCPU_INTERRUPT_STACK_SIZE == 16384 &&
           PERCPU_STRIDE == PERCPU_CONTEXT_SIZE + PERCPU_RSE_SIZE + PERCPU_STACK_SIZE +
                                PERCPU_INTERRUPT_STACK_SIZE &&
           PERCPU_STRIDE % 8192 == 0 && CONTROL_ApBlock + 8 <= PERCPU_CONTEXT_SIZE &&
           CONTROL_InterruptStack + 8 <= PERCPU_CONTEXT_SIZE &&
           CONTROL_RseRnat + 8 <= PERCPU_CONTEXT_SIZE && 4096 + 96 * 16 <= PERCPU_CONTEXT_SIZE);
    for (unsigned count = 1; count <= TABLE_MAX_CPUS; ++count)
        layout(count);

    struct ia64vmm_handoff h = {.ecam_bus_first = 1, .ecam_bus_last = 0};
    struct sal_state s;
    static struct ap_wake_block block;
    sal_init(&s, &h, 0x100000);
    /* No reachable block: refuse rather than record a vector no AP reads. */
    assert(rendez(&s, 0x240000, 0x201000, 0).status == -4 && !stored && !s.vectors[2].entry[0]);
    s.ap = &block;
    /* A zero-length rendezvous registration is valid. */
    assert(rendez(&s, 0x240000, 0x201000, 0).status == 0);
    registered(&block, 0x240000, 0x201000);
    assert(s.vectors[2].entry[0] == 0x240000 && s.vectors[2].gp[0] == 0x201000);
    assert(rendez(&s, 0x250000, 0x202000, 16).status == 0);
    registered(&block, 0x250000, 0x202000);
    /* Rejected calls leave the registration alone. */
    assert(rendez(&s, 0x260008, 0x203000, 0).status == -2 && !stored);
    stored = 0;
    assert(sal_dispatch(&s, 0x01000000, 2, 0x260000, 0x203000, 0, 0x270000, 0x203000, 0).status ==
               -2 &&
           !stored && block.rendez_entry == 0x250000 && block.rendez_gp == 0x202000);
    /* OS_MCA and OS_INIT are not AP entry points. */
    stored = 0;
    assert(sal_dispatch(&s, 0x01000000, 0, 0x210000, 0x201000, 0, 0, 0, 0).status == 0 &&
           sal_dispatch(&s, 0x01000000, 1, 0x220000, 0x201000, 0, 0x230000, 0x201000, 0).status ==
               0 &&
           !stored && block.rendez_entry == 0x250000);
    /* SAL §9: address 0 clears. */
    assert(rendez(&s, 0, 0, 0).status == 0);
    registered(&block, 0, 0);

    stored = 0;
    ap_publish(&block, UINT64_C(0x1c848000), 4);
    assert(stored == 3 && stores[0].at == &block.percpu_base && stores[0].value == 0x1c848000 &&
           !stores[0].release && stores[1].at == &block.percpu_stride &&
           stores[1].value == PERCPU_STRIDE && !stores[1].release && stores[2].at == &block.count &&
           stores[2].value == 4 && stores[2].release);
    assert(block.percpu_base == 0x1c848000 && block.percpu_stride == PERCPU_STRIDE &&
           block.count == 4);

    /* The runtime block, mapped twice: "physical" read-only for SAL once the
     * OS switched to virtual mode, so an unconverted pointer faults. */
    mapped = (size_t)sysconf(_SC_PAGESIZE);
    int fd = memfd_create("ap-test", 0);
    assert(fd >= 0 && ftruncate(fd, (off_t)mapped) == 0);
    physical = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    alias = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(physical != MAP_FAILED && alias != MAP_FAILED && physical != alias);
    volatile struct ap_wake_block *runtime_block = (volatile struct ap_wake_block *)(physical + 64);
    sal_init(&sal_runtime, &h, 0x100000);
    sal_runtime.ap = runtime_block;
    assert(sal_entry(0x01000000, 2, 0x240000, 0x201000, 0, 0, 0, 0).status == 0 &&
           runtime_block->rendez_entry == 0x240000);
    sal_virtual(NULL, NULL);
    assert(sal_runtime.ap == (volatile struct ap_wake_block *)(alias + 64));
    assert(mprotect(physical, mapped, PROT_READ) == 0);
    assert(sal_entry(0x01000000, 2, 0x280000, 0x204000, 0, 0, 0, 0).status == 0);
    assert(runtime_block->rendez_entry == 0x280000 && runtime_block->rendez_gp == 0x204000);
    /* A failed conversion must not leave the physical pointer in use. */
    sal_runtime.ap = runtime_block;
    convert_fails = TRUE;
    sal_virtual(NULL, NULL);
    assert(!sal_runtime.ap &&
           sal_entry(0x01000000, 2, 0x290000, 0x205000, 0, 0, 0, 0).status == -4 &&
           runtime_block->rendez_entry == 0x280000);
    puts(
        "AP rendezvous: OS_BOOT_RENDEZ store order, publication, area layout 1..256 and virtual ap_wake_block passed");
    return 0;
}
