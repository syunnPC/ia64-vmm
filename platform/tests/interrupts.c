/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "ia64/interrupt.h"
#include "ia64vmm/interrupts.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/platform.h"
#include "platform/acpi-irq.h"
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(e)) {                                                                                \
            fprintf(stderr, "%s:%u: %s\n", __FILE__, __LINE__, #e);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static unsigned checks, wakes[8];
static struct vcpu *cpus[3];
static bool suppress_kick;
void __real_vcpu_kick(struct vcpu *);
void __wrap_vcpu_kick(struct vcpu *v)
{
    if (!suppress_kick)
        __real_vcpu_kick(v);
}
void __wrap_hal_cpu_request(unsigned cpu, unsigned requests)
{
    if (cpu < 8 && (requests & HAL_CPU_WAKE))
        __atomic_fetch_add(&wakes[cpu], 1, __ATOMIC_RELAXED);
}
static bool host_message(uint16_t bdf, unsigned cpu, unsigned vector, unsigned count,
                         uint64_t *address, uint32_t *data)
{
    (void)bdf;
    (void)count;
    if (cpu >= 8)
        return false;
    *address = 0xfee00000u | ((uint64_t)(cpu + 17) << 12);
    *data = vector;
    return true;
}
static const struct platform_host_irq_ops direct_host = {.message = host_message};
static void clear(struct vcpu *v)
{
    memset(v->in_service, 0, sizeof(v->in_service));
    for (unsigned i = 0; i < 4; ++i)
        v->s.cr[CR_IRR0 + i] = 0;
    v->s.cr[CR_TPR] = 0;
}
static void lsapic(void)
{
    struct vcpu *v = cpus[0];
    clear(v);
    CHECK(ia64_ivr_read(v) == 15);
    CHECK(!vcpu_accept_interrupt(v, 15));
    CHECK(!vcpu_accept_interrupt(v, 256));
    vcpu_accept_interrupt(v, 0x41);
    CHECK(ia64_ivr_read(v) == 0x41);
    vcpu_accept_interrupt(v, 0x42);
    CHECK(ia64_ivr_read(v) == 0x42); /* Same class, higher vector. */
    vcpu_accept_interrupt(v, 0x40);
    CHECK(ia64_ivr_read(v) == 15);
    ia64_eoi(v);
    CHECK(ia64_ivr_read(v) == 15);
    ia64_eoi(v);
    CHECK(ia64_ivr_read(v) == 0x40);
    ia64_eoi(v);
    v->s.cr[CR_TPR] = 0x40;
    vcpu_accept_interrupt(v, 0x4f);
    CHECK(ia64_ivr_read(v) == 15);
    vcpu_accept_interrupt(v, 0x50);
    CHECK(ia64_ivr_read(v) == 0x50);
    vcpu_accept_interrupt(v, 0);
    CHECK(ia64_ivr_read(v) == 0);
    v->s.cr[CR_TPR] = 1u << 16;
    vcpu_accept_interrupt(v, 2);
    CHECK(ia64_ivr_read(v) == 2);
    vcpu_accept_interrupt(v, 2);
    CHECK(ia64_ivr_read(v) == 15);
    ia64_eoi(v);
    CHECK(ia64_ivr_read(v) == 2);
    clear(v);
    vcpu_accept_interrupt(v, 0x60);
    v->s.psr = 0;
    CHECK(!ia64_external_pending(v));
    CHECK(vcpu_halt_wake_pending(v));
    v->s.psr = IA64_PSR_I;
    CHECK(ia64_external_pending(v));
    Ia64Instruction i = {.opcode = IA64_OP_MOV_GRCR};
    i.operands.system.source = CR_LID;
    i.operands.system.destination = 8;
    ia64_gr_write(&v->s, 8, UINT64_C(0x12ab0000), false);
    CHECK(ia64_control(v, &i).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.cr[CR_LID] == UINT64_C(0x12ab0000));
    v->s.cr[CR_LID] = 0;
    CHECK(platform_irq_update_lid(v, 0));
    clear(v);
    const unsigned regs[] = {CR_ITV, CR_PMV, CR_CMCV, CR_LRR0, CR_LRR1};
    for (unsigned n = 0; n < 5; ++n) {
        v->s.cr[regs[n]] = (1u << 16) | 0x68;
        CHECK(!vcpu_raise_local_interrupt(v, n));
        CHECK(ia64_ivr_read(v) == 15);
        v->s.cr[regs[n]] = 0x68;
        CHECK(vcpu_raise_local_interrupt(v, n));
        CHECK(ia64_ivr_read(v) == 0x68);
        ia64_eoi(v);
    }
    v->s.cr[CR_LRR0] = 4u << 8;
    CHECK(vcpu_raise_local_interrupt(v, VCPU_LOCAL_LINT0));
    CHECK(ia64_ivr_read(v) == 2);
    ia64_eoi(v);
    v->s.cr[CR_LRR1] = 7u << 8;
    CHECK(vcpu_raise_local_interrupt(v, VCPU_LOCAL_LINT1));
    CHECK(ia64_ivr_read(v) == 0);
    ia64_eoi(v);
}
static void pib(void)
{
    struct vcpu *v = cpus[0];
    platform_pib_ops.write(NULL, v, IA64_PIB_XTP, 0xad, 1);
    CHECK(platform_irq_xtp(v) == 0xad);
    CHECK(platform_pib_ops.read(NULL, v, IA64_PIB_XTP, 1) == 0xad);
    CHECK(platform_pib_ops.read(NULL, v, IA64_PIB_INTA, 1) == 15);
    platform_pib_ops.write(NULL, v, 0, 0x61, 8);
    CHECK(v->s.cr[CR_IRR0 + 1] == 0); /* Mailbox keeps IRQ mutation outside engine slices. */
    platform_irq_poll(v);
    CHECK(ia64_ivr_read(v) == 0x61);
    ia64_eoi(v);
    platform_pib_ops.write(NULL, v, 0x1000, 0x62, 8);
    platform_irq_poll(cpus[1]);
    CHECK(ia64_ivr_read(cpus[1]) == 0x62);
    ia64_eoi(cpus[1]);
    CHECK(wakes[3]);
    platform_pib_ops.write(NULL, v, 0x2010, 0x63, 8);
    platform_irq_poll(cpus[2]);
    CHECK(ia64_ivr_read(cpus[2]) == 0x63);
    ia64_eoi(cpus[2]);
    platform_pib_ops.write(NULL, v, 8, 0x64, 8);
    platform_irq_poll(v);
    CHECK(ia64_ivr_read(v) == 15);
    platform_pib_ops.write(NULL, v, 0, 4u << 8, 8);
    platform_irq_poll(v);
    CHECK(ia64_ivr_read(v) == 2);
    ia64_eoi(v);
    platform_pib_ops.write(NULL, v, 0, 7u << 8, 8);
    platform_irq_poll(v);
    CHECK(ia64_ivr_read(v) == 0);
    ia64_eoi(v);
    /* INIT (101b) and PMI (010b, vectors 0-15) reach the target's PAL-based
     * interruption latches through the mailbox at its poll; without a
     * PALE_INIT or a registered PALE_PMI the target stops. The vector of an INIT message is ignored. */
    platform_pib_ops.write(NULL, v, 0x2010, 5u << 8, 8);
    platform_irq_poll(cpus[2]);
    CHECK(cpus[2]->requested_exit == VCPU_EXIT_UNIMPLEMENTED + 1);
    cpus[2]->requested_exit = 0;
    CHECK(!vcpu_pal_interrupt_pending(cpus[2]));
    platform_pib_ops.write(NULL, v, 0x2010, 2u << 8 | 3, 8);
    platform_irq_poll(cpus[2]);
    CHECK(cpus[2]->requested_exit == VCPU_EXIT_UNIMPLEMENTED + 1);
    cpus[2]->requested_exit = 0;
    struct vcpu *t = cpus[1];
    t->cfg.pale_init = 0x7000;
    ia64_pal_service_write(t, IA64VMM_SVC_PAL_PALE_PMI, 0x7200);
    unsigned before = wakes[3];
    platform_pib_ops.write(NULL, v, 0x1000, 5u << 8 | 0x99, 8);
    CHECK(!vcpu_pal_interrupt_pending(t) && wakes[3] > before);
    platform_irq_poll(t);
    CHECK(t->pal_init_pending && !t->pal_pmi_pending && !t->requested_exit);
    platform_pib_ops.write(NULL, v, 0x1000, 2u << 8 | 15, 8);
    platform_pib_ops.write(NULL, v, 0x1000, 2u << 8 | 0, 8);
    platform_pib_ops.write(NULL, v, 0x1000, 2u << 8 | 16, 8);
    platform_irq_poll(t);
    CHECK(t->pal_pmi_pending == ((1u << 15) | 1u) && !t->requested_exit);
    CHECK(ia64_ivr_read(t) == 15); /* Never through IRR. */
    CHECK(!platform_irq_inject(1, 16, IA64_IRQ_PMI) && platform_irq_inject(1, 7, IA64_IRQ_PMI));
    platform_irq_poll(t);
    CHECK(t->pal_pmi_pending == ((1u << 15) | (1u << 7) | 1u));
    t->pal_init_pending = false;
    t->pal_pmi_pending = 0;
    t->cfg.pale_init = 0;
    ia64_pal_service_write(t, IA64VMM_SVC_PAL_PALE_PMI, 0);
    CHECK(!platform_irq_inject(3, 0x64, IA64_IRQ_INT));
}
static void reg(struct ia64_iosapic *s, unsigned index, uint32_t value)
{
    platform_iosapic_ops.write(s, cpus[0], 0, index, 4);
    platform_iosapic_ops.write(s, cpus[0], 0x10, value, 4);
    platform_intx_poll(cpus[0]);
}
static uint32_t readreg(struct ia64_iosapic *s, unsigned index)
{
    platform_iosapic_ops.write(s, cpus[0], 0, index, 4);
    return platform_iosapic_ops.read(s, cpus[0], 0x10, 4);
}
static void iosapic(void)
{
    struct ia64_iosapic s;
    CHECK(platform_iosapic_init(&s, 0, 24));
    CHECK((readreg(&s, 1) >> 16) == 23);
    CHECK((readreg(&s, 1) & 255) >= 0x11);
    unsigned index = 0x10 + 9 * 2;
    reg(&s, index, 0x71 | IA64_IOSAPIC_LEVEL | IA64_IOSAPIC_MASK);
    CHECK(platform_iosapic_set_pin(&s, 9, true));
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 15);
    reg(&s, index, 0x71 | IA64_IOSAPIC_LEVEL);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x71);
    CHECK(readreg(&s, index) & IA64_IOSAPIC_REMOTE_IRR);
    ia64_eoi(cpus[0]);
    CHECK(readreg(&s, index) & IA64_IOSAPIC_REMOTE_IRR); /* Local EOI is separate. */
    platform_iosapic_ops.write(&s, cpus[0], 0x40, 0x71, 4);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x71);
    ia64_eoi(cpus[0]);
    CHECK(platform_iosapic_set_pin(&s, 9, false));
    platform_iosapic_ops.write(&s, cpus[0], 0x40, 0x71, 4);
    CHECK(!(readreg(&s, index) & IA64_IOSAPIC_REMOTE_IRR));
    reg(&s, index, 0x72);
    platform_iosapic_set_pin(&s, 9, true);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x72);
    ia64_eoi(cpus[0]);
    platform_iosapic_set_pin(&s, 9, true);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 15);
    reg(&s, index, 0x72 | IA64_IOSAPIC_MASK);
    platform_iosapic_set_pin(&s, 9, false);
    platform_iosapic_set_pin(&s, 9, true);
    reg(&s, index, 0x72);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 15);
    reg(&s, index + 1, 0x02010000);
    reg(&s, index, 0x73 | IA64_IOSAPIC_LEVEL | (1u << 13));
    platform_iosapic_set_pin(&s, 9, false);
    platform_irq_poll(cpus[2]);
    CHECK(ia64_ivr_read(cpus[2]) == 0x73);
    ia64_eoi(cpus[2]);
    /* RTE delivery modes PMI and INIT (I/O SAPIC: 010b, 101b). */
    struct vcpu *t = cpus[1];
    t->cfg.pale_init = 0x7000;
    ia64_pal_service_write(t, IA64VMM_SVC_PAL_PALE_PMI, 0x7200);
    reg(&s, 0x10 + 2 * 5 + 1, 0x01000000);
    reg(&s, 0x10 + 2 * 5, 2u << 8 | 2);
    platform_iosapic_set_pin(&s, 5, true);
    platform_irq_poll(t);
    CHECK(t->pal_pmi_pending == (1u << 2) && !t->pal_init_pending);
    reg(&s, 0x10 + 2 * 6 + 1, 0x01000000);
    reg(&s, 0x10 + 2 * 6, 5u << 8);
    platform_iosapic_set_pin(&s, 6, true);
    platform_irq_poll(t);
    CHECK(t->pal_init_pending && ia64_ivr_read(t) == 15);
    t->pal_init_pending = false;
    t->pal_pmi_pending = 0;
    t->cfg.pale_init = 0;
    ia64_pal_service_write(t, IA64VMM_SVC_PAL_PALE_PMI, 0);
    CHECK(!platform_iosapic_set_pin(&s, 24, true));
}
static void translation(void)
{
    platform_irq_set_host(&direct_host);
    struct ia64_irq_message m;
    CHECK(platform_msi_decode(0xfee01000, 0x63, &m));
    CHECK(m.cpu == 1 && m.vector == 0x63);
    CHECK(platform_msi_decode(0xfee02010, 0xc064, &m));
    CHECK(m.cpu == 2 && m.level);
    CHECK(platform_irq_relocate(1, 5));
    CHECK(!platform_msi_decode(0xfee03000, 0x61, &m));
    CHECK(!platform_msi_decode(0xfee00000, 0xe1, &m));
    CHECK(!platform_msi_decode(0xfee00000, 15, &m));
    CHECK(!platform_msi_decode(0xfee00008, 0x61, &m));
    CHECK(!platform_msi_decode(0x1fee00000ULL, 0x61, &m));
}
static uint8_t host_config[256];
static uint32_t config_read(void *p, uint16_t bdf, unsigned off, unsigned n)
{
    (void)p;
    (void)bdf;
    uint32_t value = 0;
    for (unsigned i = 0; i < n; ++i)
        value |= (uint32_t)host_config[off + i] << (8 * i);
    return value;
}
static void config_write(void *p, uint16_t bdf, unsigned off, uint32_t value, unsigned n)
{
    (void)p;
    (void)bdf;
    for (unsigned i = 0; i < n; ++i)
        host_config[off + i] = value >> (8 * i);
}
static void put(struct pci_device *d, unsigned off, uint32_t value, unsigned n)
{
    for (unsigned i = 0; i < n; ++i)
        d->irq_config[off + i] = value >> (8 * i);
}
static void mediation(void)
{
    struct pci_device d = {.bdf = 0x18,
                           .owner = PCI_OWNER_GUEST,
                           .storage = true,
                           .msi_offset = 0x40,
                           .msi_length = 24,
                           .msix_offset = 0x60};
    const struct pci_host_ops host = {config_read, config_write, NULL};
    d.bars[0].base = 0x80000000;
    d.bars[0].size = 0x4000;
    d.bars[0].hva = hal_alloc_pages(4, HAL_PAGE_ZERO);
    CHECK(d.bars[0].hva);
    put(&d, 0x62, 1, 2);
    put(&d, 0x64, 0x1000, 4);
    put(&d, 0x68, 0x1800, 4);
    CHECK(platform_msi_prepare(&d, &host, NULL));
    uint64_t end = d.bars[0].base + d.bars[0].size;
    void *opaque;
    const struct gm_mmio_ops *ops =
        platform_msi_bar_region(&d, &d.bars[0], d.bars[0].base + 0x1000, &end, &opaque);
    CHECK(ops && end == d.bars[0].base + 0x2000);
    ops->write(opaque, cpus[0], 0, 0xfee00000, 8);
    ops->write(opaque, cpus[0], 8, 0x65, 4);
    CHECK(ops->read(opaque, cpus[0], 0, 8) == 0xfee00000);
    put(&d, 0x42, 0x1a5, 2); /* Guest retains an enabled four-message MSI tuple. */
    put(&d, 0x62, 0x8001, 2);
    platform_msi_config_changed(&d);
    CHECK(!(config_read(NULL, d.bdf, 0x42, 2) & 0x71));
    CHECK(d.irq_config[0x42] == 0xa5);
    ops->write(opaque, cpus[0], 12, 0, 4);
    CHECK(*(uint32_t *)(d.bars[0].hva + 0x1000) == 0xfee13000);
    unsigned vector = *(uint32_t *)(d.bars[0].hva + 0x1008);
    CHECK(vector >= 0x40 && vector <= 0xdf);
    CHECK(platform_irq_relocate(0, 7));
    CHECK(*(uint32_t *)(d.bars[0].hva + 0x1000) == 0xfee18000);
    CHECK(ops->read(opaque, cpus[0], 0, 8) == 0xfee00000);
    CHECK(platform_irq_relocate(0, 2));
    CHECK(*(uint32_t *)(d.bars[0].hva + 0x1000) == 0xfee13000);
    struct code_validity_stats before, after;
    code_validity_get_stats(&before);
    CHECK(platform_msi_interrupt(vector));
    code_validity_get_stats(&after);
    CHECK(after.epoch_bumps == before.epoch_bumps + 1);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x65);
    ia64_eoi(cpus[0]);
    ops->write(opaque, cpus[0], 12, 1, 4);
    platform_msi_interrupt(vector);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 15);
    CHECK(ops->read(opaque, cpus[0], 0x800, 8) & 1);
    ops->write(opaque, cpus[0], 12, 0, 4);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x65);
    ia64_eoi(cpus[0]);
    put(&d, 0x62, 0xc001, 2);
    platform_msi_config_changed(&d);
    platform_msi_interrupt(vector);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 15);
    CHECK(ops->read(opaque, cpus[0], 0x800, 8) & 1);
    code_validity_get_stats(&before);
    put(&d, 0x62, 0x8001, 2);
    platform_msi_config_changed(&d);
    code_validity_get_stats(&after);
    CHECK(before.epoch_bumps == after.epoch_bumps);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x65);
    ia64_eoi(cpus[0]);
    *(uint64_t *)(d.bars[0].hva + 0x1800) = 2;
    CHECK(ops->read(opaque, cpus[0], 0x800, 8) == 2);
    ops->write(opaque, cpus[0], 0x800, 0, 8);
    CHECK(*(uint64_t *)(d.bars[0].hva + 0x1800) == 2);
    *(uint64_t *)(d.bars[0].hva + 0x1800) = 0;
    ops->write(opaque, cpus[0], 0x900, 0x12345678, 4);
    CHECK(*(uint32_t *)(d.bars[0].hva + 0x1900) == 0x12345678);
    CHECK(ops->read(opaque, cpus[0], 0x900, 4) == 0x12345678);
    ops->write(opaque, cpus[0], 0x908, UINT64_C(0x5a5a5a5a5a5a5a5a), 8);
    ops->write(opaque, cpus[0], 0x90b, 0x87, 1);
    ops->write(opaque, cpus[0], 0x908, 0x7654, 2);
    CHECK(ops->read(opaque, cpus[0], 0x90b, 1) == 0x87);
    CHECK(ops->read(opaque, cpus[0], 0x908, 2) == 0x7654);
    CHECK(ops->read(opaque, cpus[0], 0x908, 8) == UINT64_C(0x5a5a5a5a875a7654));
    put(&d, 0x62, 1, 2);
    put(&d, 0x42, 0x181, 2);
    put(&d, 0x44, 0xfee00000, 4);
    put(&d, 0x4c, 0x66, 2);
    platform_msi_config_changed(&d);
    CHECK(config_read(NULL, d.bdf, 0x42, 2) & 1);
    CHECK(config_read(NULL, d.bdf, 0x44, 4) == 0xfee13000);
    vector = config_read(NULL, d.bdf, 0x4c, 2);
    CHECK(vector >= 0x40 && vector <= 0xdf);
    platform_msi_interrupt(vector);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x66);
    ia64_eoi(cpus[0]);
    CHECK(platform_irq_relocate(0, 6));
    CHECK(config_read(NULL, d.bdf, 0x44, 4) == 0xfee17000);
    CHECK(platform_irq_relocate(0, 2));
    put(&d, 0x50, 1, 4);
    platform_msi_config_changed(&d);
    platform_msi_interrupt(vector);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 15);
    platform_msi_sync_pending(&d);
    CHECK(d.irq_config[0x54] & 1);
    put(&d, 0x50, 0, 4);
    platform_msi_config_changed(&d);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x66);
    ia64_eoi(cpus[0]);
    put(&d, 0x42, 0x1a5, 2);
    put(&d, 0x4c, 0x70, 2);
    platform_msi_config_changed(&d);
    vector = config_read(NULL, d.bdf, 0x4c, 2);
    CHECK(!(vector & 3));
    for (unsigned n = 0; n < 4; ++n) {
        platform_msi_interrupt(vector + n);
        platform_irq_poll(cpus[0]);
        CHECK(ia64_ivr_read(cpus[0]) == 0x70 + n);
        ia64_eoi(cpus[0]);
    }
    put(&d, 0x42, 0x181, 2);
    put(&d, 0x4c, 0xe1, 2);
    platform_msi_config_changed(&d);
    CHECK(!(config_read(NULL, d.bdf, 0x42, 2) & 1));
    d.storage = false;
    code_validity_get_stats(&before);
    platform_msi_interrupt(vector);
    code_validity_get_stats(&after);
    CHECK(after.epoch_bumps == before.epoch_bumps);
    platform_msi_cleanup(&d);
    hal_free_pages(d.bars[0].hva, 4);
}
static uint16_t irq_source[256];
static unsigned irq_programs, irq_first, irq_count, irq_retirements, queued_irq;
static unsigned retired_guest_vector = 15;
static bool irq_refuse;
static bool remapped_message(uint16_t bdf, unsigned cpu, unsigned first, unsigned count,
                             uint64_t *address, uint32_t *data)
{
    CHECK(bdf == 0x1238 && cpu == 2 && count && count <= 32);
    CHECK(!(config_read(NULL, bdf, 0x42, 2) & 1));
    ++irq_programs;
    irq_first = first;
    irq_count = count;
    if (irq_refuse)
        return false;
    for (unsigned i = 0; i < count; ++i)
        irq_source[first + i] = bdf;
    *address = UINT64_C(0xfee00018) | ((uint64_t)first << 5);
    *data = 0;
    return true;
}
static void revoke_message(unsigned first, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        irq_source[first + i] = 0;
}
static void retire_messages(const uint64_t vectors[4])
{
    ++irq_retirements;
    for (unsigned v = 0; v < 256; ++v)
        if (vectors[v / 64] & (UINT64_C(1) << (v % 64)))
            CHECK(!irq_source[v]);
    if (!queued_irq)
        return;
    CHECK(vectors[queued_irq / 64] & (UINT64_C(1) << (queued_irq % 64)));
    unsigned owner;
    unsigned held = platform_irq_vector_reserve(&owner, 1);
    CHECK(held && !(vectors[held / 64] & (UINT64_C(1) << (held % 64))));
    unsigned old = queued_irq;
    queued_irq = 0;
    platform_msi_interrupt(old);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == retired_guest_vector);
    if (retired_guest_vector != 15)
        ia64_eoi(cpus[0]);
    platform_irq_vector_release(&owner);
}
static void remapping(void)
{
    /* PCI MSI multiple messages use one programmed tuple; VT-d 5.1.2 adds
     * the device subhandle to its table index. Reuse follows IEC draining
     * and host dispatch completion (VT-d 6.5.5). */
    static const struct platform_host_irq_ops remapped_host = {
        .message = remapped_message,
        .revoke = revoke_message,
        .retire = retire_messages,
        .remapped = true,
    };
    platform_irq_set_host(&remapped_host);
    struct pci_device d = {
        .bdf = 0x1238, .owner = PCI_OWNER_GUEST, .msi_offset = 0x40, .msi_length = 24};
    const struct pci_host_ops host = {config_read, config_write, NULL};
    memset(host_config, 0, sizeof(host_config));
    CHECK(platform_msi_prepare(&d, &host, NULL));
    put(&d, 0x42, 0x1a7, 2);
    put(&d, 0x44, 0xfee00000, 4);
    put(&d, 0x4c, 0x70, 2);
    platform_msi_config_changed(&d);
    CHECK(irq_programs == 1 && irq_count == 4 && !(irq_first & 3));
    CHECK(config_read(NULL, d.bdf, 0x42, 2) & 1);
    CHECK(config_read(NULL, d.bdf, 0x44, 4) == (0xfee00018u | (irq_first << 5)));
    CHECK(config_read(NULL, d.bdf, 0x4c, 2) == 0);
    unsigned previous = irq_first;
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(irq_source[irq_first + i] == d.bdf);
        platform_msi_interrupt(irq_first + i);
        platform_irq_poll(cpus[0]);
        CHECK(ia64_ivr_read(cpus[0]) == 0x70 + i);
        ia64_eoi(cpus[0]);
    }
    queued_irq = irq_first;
    retired_guest_vector = 0x70;
    put(&d, 0x4c, 0x78, 2);
    platform_msi_config_changed(&d);
    CHECK(!queued_irq && irq_programs == 2);
    retired_guest_vector = 15;
    put(&d, 0x42, 0x1b7, 2);
    platform_msi_config_changed(&d);
    CHECK(irq_programs == 3 && irq_count == 8);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(!irq_source[previous + i]);
    put(&d, 0x4c, 0xe0, 2);
    platform_msi_config_changed(&d);
    CHECK(irq_programs == 3 && !(config_read(NULL, d.bdf, 0x42, 2) & 1));
    for (unsigned i = 0; i < 8; ++i)
        CHECK(!irq_source[irq_first + i]);
    put(&d, 0x4c, 0x70, 2);
    irq_refuse = true;
    platform_msi_config_changed(&d);
    CHECK(irq_programs == 4 && !(config_read(NULL, d.bdf, 0x42, 2) & 1));
    irq_refuse = false;
    platform_msi_config_changed(&d);
    queued_irq = irq_first;
    platform_msi_restart(&d);
    CHECK(!queued_irq && irq_retirements);
    for (unsigned v = 0; v < 256; ++v)
        CHECK(!irq_source[v]);
    platform_msi_config_changed(&d);
    CHECK(irq_first == previous);
    platform_msi_cleanup(&d);
    platform_irq_set_host(&direct_host);
}
/* A retired IOAPIC delivery may still reach its software binding. */
static bool line_present[256], queued_line[256], host_line_masked[64];
static uint32_t line_gsi[256];
static unsigned line_retirements;
static void line_revoke(unsigned first, unsigned count)
{
    for (unsigned i = first; i < first + count; ++i) {
        if (!line_gsi[i])
            continue;
        CHECK(line_gsi[i] < 64 && host_line_masked[line_gsi[i]]);
        line_present[i] = false;
    }
}
static void line_retire(const uint64_t vectors[4])
{
    ++line_retirements;
    for (unsigned i = 0; i < 256; ++i)
        if (vectors[i / 64] & (UINT64_C(1) << (i % 64))) {
            if (!line_gsi[i])
                continue;
            CHECK(!line_present[i] && host_line_masked[line_gsi[i]]);
            if (queued_line[i]) {
                CHECK(platform_intx_interrupt(i));
                queued_line[i] = false;
            }
        }
}
static const struct platform_host_irq_ops line_host = {.revoke = line_revoke,
                                                       .retire = line_retire};
static unsigned stage;
static bool line_masked;
static unsigned line_vector;
static uint16_t intx_command[2];
static bool intx_resolve(void *p, uint16_t bdf, unsigned pin, uint32_t *gsi, bool *low, bool *level)
{
    (void)p;
    (void)bdf;
    CHECK(pin == 1);
    *gsi = 37;
    *low = true;
    *level = true;
    return true;
}
static bool intx_route(void *p, uint32_t gsi, unsigned cpu, unsigned vector, bool low)
{
    (void)p;
    CHECK(gsi == 37 && cpu == 0 && low && !queued_line[vector]);
    line_vector = vector;
    line_masked = host_line_masked[gsi] = true;
    line_gsi[vector] = gsi;
    line_present[vector] = true;
    return true;
}
static void intx_mask(void *p, uint32_t gsi, bool masked)
{
    (void)p;
    CHECK(gsi == 37);
    line_masked = host_line_masked[gsi] = masked;
}
static void intx_unmask_if_idle(void *p, uint32_t gsi, const unsigned *waiting)
{
    intx_mask(p, gsi, __atomic_load_n(waiting, __ATOMIC_ACQUIRE));
}
static void intx_write(void *p, uint16_t bdf, unsigned off, uint32_t value, unsigned n)
{
    (void)p;
    CHECK(off == 4 && n == 2 && bdf < 2);
    intx_command[bdf] = value;
}
static void intx(void)
{
    static const struct platform_intx_ops backend = {intx_resolve, intx_route, intx_mask,
                                                     intx_unmask_if_idle, NULL};
    static const struct pci_host_ops host = {.write = intx_write};
    struct ia64_iosapic sapic;
    CHECK(platform_iosapic_init(&sapic, 0, 24));
    struct pci_device a = {.bdf = 0, .irq_pin = 1, .command = 0x400, .storage = true};
    struct pci_device b = {.bdf = 1, .irq_pin = 1, .command = 0x400, .msi_offset = 0x40};
    platform_irq_set_host(&line_host);
    platform_intx_set_host(&backend, NULL);
    CHECK(platform_intx_prepare(&a, &host, NULL, &sapic));
    CHECK(platform_intx_prepare(&b, &host, NULL, &sapic));
    CHECK(a.irq_line == 16 && b.irq_line == 16 && line_masked);
    CHECK(sapic.input[16]); /* PCI inactive level is high. */
    unsigned other = platform_irq_vector_reserve(&other, 1);
    CHECK(other && other != line_vector);
    platform_irq_vector_release(&other);
    reg(&sapic, 0x30, 0x75 | IA64_IOSAPIC_LEVEL | (1u << 13));
    CHECK(line_masked);
    a.command = 0;
    platform_intx_config_changed(&a);
    platform_intx_poll(cpus[0]);
    CHECK(!line_masked && !intx_command[0]);
    struct code_validity_stats before, after;
    code_validity_get_stats(&before);
    CHECK(platform_intx_interrupt(line_vector));
    CHECK(line_masked);
    CHECK(!(sapic.rte[16] & IA64_IOSAPIC_REMOTE_IRR)); /* IRQ never mutates guest state. */
    platform_irq_poll(cpus[0]);
    CHECK(sapic.rte[16] & IA64_IOSAPIC_REMOTE_IRR);
    CHECK(!sapic.input[16]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x75);
    code_validity_get_stats(&after);
    CHECK(after.epoch_bumps == before.epoch_bumps + 1);
    platform_iosapic_ops.write(&sapic, cpus[0], 0x40, 0x74, 4);
    CHECK(line_masked);
    ia64_eoi(cpus[0]);
    CHECK(line_masked); /* EOI hardware work is deferred past lockstep. */
    platform_irq_poll(cpus[0]);
    CHECK(!line_masked && !(sapic.rte[16] & IA64_IOSAPIC_REMOTE_IRR));
    CHECK(ia64_ivr_read(cpus[0]) == 15);
    b.command = 0;
    platform_intx_config_changed(&b);
    platform_intx_poll(cpus[0]);
    a.command = 0x400;
    platform_intx_config_changed(&a);
    platform_intx_poll(cpus[0]);
    CHECK(!line_masked && intx_command[0] == 0x400 &&
          !intx_command[1]); /* Shared peer stays enabled. */
    CHECK(platform_intx_interrupt(line_vector));
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x75);
    platform_iosapic_ops.write(&sapic, cpus[0], 0x40, 0x75, 4);
    platform_irq_poll(cpus[0]);
    CHECK(!line_masked);
    ia64_eoi(cpus[0]);
    b.irq_config[0x42] = 1;
    platform_intx_config_changed(&b);
    platform_intx_poll(cpus[0]);
    CHECK(line_masked && (intx_command[1] & 0x400));
    b.irq_config[0x42] = 0;
    platform_intx_config_changed(&b);
    platform_intx_poll(cpus[0]);
    CHECK(!line_masked);
    reg(&sapic, 0x30, 0x75 | IA64_IOSAPIC_LEVEL | (1u << 13) | IA64_IOSAPIC_MASK);
    CHECK(line_masked);
    reg(&sapic, 0x30, 0x75 | IA64_IOSAPIC_LEVEL | IA64_IOSAPIC_MASK);
    CHECK(line_masked && sapic.input[16]);
    reg(&sapic, 0x30, 0x75 | IA64_IOSAPIC_LEVEL | (1u << 13));
    CHECK(!line_masked);
    unsigned retired = line_retirements;
    queued_line[line_vector] = true;
    platform_intx_restart();
    CHECK(!queued_line[line_vector] && line_retirements == retired + 1);
    CHECK(line_masked && line_present[line_vector]);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 15);
    platform_intx_config_changed(&b);
    platform_irq_poll(cpus[0]);
    CHECK(!line_masked && ia64_ivr_read(cpus[0]) == 15);
    CHECK(platform_intx_interrupt(line_vector));
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x75);
    platform_iosapic_ops.write(&sapic, cpus[0], 0x40, 0x75, 4);
    ia64_eoi(cpus[0]);
    platform_irq_poll(cpus[0]);
    platform_intx_cleanup();
    CHECK(line_masked);
    platform_intx_set_host(NULL, NULL);
    platform_irq_set_host(&direct_host);
    clear(cpus[0]);
}
/* ISA lines: MADT overrides, edge pulses, guest-mask tracking. */
static void madt_entry(uint8_t *t, unsigned *at, const uint8_t *e, unsigned n)
{
    memcpy(t + *at, e, n);
    *at += n;
    t[4] = *at;
    t[5] = *at >> 8;
}
static void isa_overrides(void)
{
    uint8_t t[128] = {'A', 'P', 'I', 'C'};
    unsigned at = 44;
    uint32_t gsi;
    bool low, level;
    const uint8_t lapic[8] = {0, 8, 0, 0, 1}, ioapic[12] = {1, 12, 0, 0, 0, 0, 0xc0, 0xfe};
    const uint8_t irq0[10] = {2, 10, 0, 0, 2}, irq9[10] = {2, 10, 0, 9, 9, 0, 0, 0, 0x0d};
    madt_entry(t, &at, lapic, 8);
    madt_entry(t, &at, ioapic, 12);
    madt_entry(t, &at, irq0, 10);
    madt_entry(t, &at, irq9, 10); /* QEMU q35's own overrides. */
    CHECK(acpi_isa_irq(t, 14, &gsi, &low, &level) && gsi == 14 && !low && !level);
    CHECK(acpi_isa_irq(t, 0, &gsi, &low, &level) && gsi == 2 && !low && !level);
    CHECK(acpi_isa_irq(t, 9, &gsi, &low, &level) && gsi == 9 && !low && level);
    CHECK(!acpi_isa_irq(t, 16, &gsi, &low, &level) && !acpi_isa_irq(NULL, 1, &gsi, &low, &level));
    const uint8_t irq14[10] = {2, 10, 0, 14, 20, 0, 0, 0, 0x07};
    madt_entry(t, &at, irq14, 10); /* Edge, active low, moved to GSI 20. */
    CHECK(acpi_isa_irq(t, 14, &gsi, &low, &level) && gsi == 20 && low && !level);
    t[at - 2] = 0x02; /* Reserved polarity encoding. */
    CHECK(!acpi_isa_irq(t, 14, &gsi, &low, &level));
    t[at - 2] = 0x07;
    t[at - 9] = 9; /* Malformed override length. */
    CHECK(!acpi_isa_irq(t, 1, &gsi, &low, &level));
    t[at - 9] = 10;
    t[4] = at - 3; /* Truncated table. */
    CHECK(!acpi_isa_irq(t, 1, &gsi, &low, &level));
}
static unsigned isa_vector[16];
static bool isa_masked[64];
static bool isa_route(void *p, unsigned irq, unsigned cpu, unsigned vector, uint32_t *gsi)
{
    (void)p;
    CHECK(cpu == 0 && irq < 16);
    if (irq == 15)
        return false; /* A host line with a level override. */
    CHECK(!queued_line[vector]);
    isa_vector[irq] = vector;
    *gsi = irq + 20;
    isa_masked[*gsi] = host_line_masked[*gsi] = true;
    line_gsi[vector] = *gsi;
    line_present[vector] = true;
    return true;
}
static void isa_mask(void *p, uint32_t gsi, bool masked)
{
    (void)p;
    CHECK(gsi < 64);
    isa_masked[gsi] = host_line_masked[gsi] = masked;
}
static void isa(void)
{
    static const struct platform_intx_ops backend = {intx_resolve, intx_route, isa_mask, NULL,
                                                     isa_route};
    struct ia64_iosapic sapic;
    CHECK(platform_iosapic_init(&sapic, 0, 24));
    struct code_validity_stats before, after;
    platform_irq_set_host(&line_host);
    platform_intx_set_host(NULL, NULL);
    CHECK(
        platform_intx_prepare_isa(14, &sapic, true)); /* No host IOAPIC: warned, nothing routed. */
    platform_intx_set_host(&backend, NULL);
    CHECK(!platform_intx_prepare_isa(IA64VMM_SCI_GSI, &sapic, false));
    CHECK(!platform_intx_prepare_isa(16, &sapic, false));
    CHECK(platform_intx_prepare_isa(1, &sapic, false));
    CHECK(platform_intx_prepare_isa(14, &sapic, false));
    CHECK(platform_intx_prepare_isa(14, &sapic, true)); /* One line, storage added. */
    CHECK(platform_intx_prepare_isa(15, &sapic, true) && !isa_vector[15]);
    CHECK(isa_vector[1] && isa_vector[14] && isa_vector[1] != isa_vector[14]);
    CHECK(isa_masked[21] && isa_masked[34]);
    /* Edges at a masked guest pin are discarded, as on an IOSAPIC. */
    CHECK(platform_intx_interrupt(isa_vector[14]));
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 15);
    reg(&sapic, 0x2c, 0x81);
    CHECK(!isa_masked[34] && isa_masked[21]);
    code_validity_get_stats(&before);
    CHECK(platform_intx_interrupt(isa_vector[14]));
    CHECK(!isa_masked[34]); /* Edge: never masked. */
    code_validity_get_stats(&after);
    CHECK(after.epoch_bumps == before.epoch_bumps + 1);
    CHECK(ia64_ivr_read(cpus[0]) == 15); /* The IRQ path never mutates guest state. */
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x81 && !sapic.input[14]);
    ia64_eoi(cpus[0]);
    CHECK(platform_intx_interrupt(isa_vector[14]) && platform_intx_interrupt(isa_vector[14]));
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x81);
    ia64_eoi(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 15); /* Edges before one poll coalesce. */
    reg(&sapic, 0x12, 0x82);
    CHECK(!isa_masked[21]);
    code_validity_get_stats(&before);
    CHECK(platform_intx_interrupt(isa_vector[1]));
    platform_irq_poll(cpus[0]);
    code_validity_get_stats(&after);
    CHECK(after.epoch_bumps == before.epoch_bumps);
    CHECK(ia64_ivr_read(cpus[0]) == 0x82);
    ia64_eoi(cpus[0]);
    reg(&sapic, 0x2c, 0x81 | IA64_IOSAPIC_MASK);
    CHECK(isa_masked[34]);
    reg(&sapic, 0x2d, 0x7f000000);
    reg(&sapic, 0x2c, 0x81);
    CHECK(isa_masked[34]); /* No such CPU. */
    reg(&sapic, 0x2d, 0);
    reg(&sapic, 0x2c, 0x81);
    CHECK(!isa_masked[34]);
    unsigned retired = line_retirements;
    queued_line[isa_vector[1]] = queued_line[isa_vector[14]] = true;
    platform_intx_restart();
    CHECK(!queued_line[isa_vector[1]] && !queued_line[isa_vector[14]] &&
          line_retirements == retired + 2);
    CHECK(isa_masked[21] && isa_masked[34] && line_present[isa_vector[1]] &&
          line_present[isa_vector[14]]);
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 15);
    reg(&sapic, 0x12, 0x82);
    reg(&sapic, 0x2c, 0x81);
    platform_irq_poll(cpus[0]);
    CHECK(!isa_masked[21] && !isa_masked[34] && ia64_ivr_read(cpus[0]) == 15);
    CHECK(platform_intx_interrupt(isa_vector[14]));
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x81);
    ia64_eoi(cpus[0]);
    CHECK(platform_intx_interrupt(isa_vector[1]));
    platform_irq_poll(cpus[0]);
    CHECK(ia64_ivr_read(cpus[0]) == 0x82);
    ia64_eoi(cpus[0]);
    platform_intx_cleanup();
    CHECK(isa_masked[21] && isa_masked[34]);
    CHECK(!platform_intx_interrupt(isa_vector[14]));
    platform_intx_set_host(NULL, NULL);
    platform_irq_set_host(&direct_host);
    clear(cpus[0]);
}
static void *sender(void *unused)
{
    (void)unused;
    for (unsigned i = 1; i <= 10000; ++i) {
        while (__atomic_load_n(&stage, __ATOMIC_ACQUIRE) != (i - 1) * 2) {
        }
        if (!platform_irq_inject(1, 0x67, IA64_IRQ_INT))
            abort();
        __atomic_store_n(&stage, i * 2 - 1, __ATOMIC_RELEASE);
        while (__atomic_load_n(&stage, __ATOMIC_ACQUIRE) != i * 2) {
        }
    }
    return NULL;
}
static void concurrent(void)
{
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, sender, NULL));
    for (unsigned i = 1; i <= 10000; ++i) {
        CHECK(platform_irq_relocate(1, (i & 1) ? 3 : 5));
        while (__atomic_load_n(&stage, __ATOMIC_ACQUIRE) != i * 2 - 1) {
        }
        platform_irq_poll(cpus[1]);
        CHECK(ia64_ivr_read(cpus[1]) == 0x67);
        ia64_eoi(cpus[1]);
        __atomic_store_n(&stage, i * 2, __ATOMIC_RELEASE);
    }
    CHECK(!pthread_join(thread, NULL));
}
static uint64_t injected_at, injected_retired, ivt_at, ivt_retired;
static bool latency_hook(struct vcpu *v, uint64_t imm, uint64_t ip)
{
    (void)ip;
    if (imm == 1 && !injected_at) {
        injected_at = hal_counter();
        injected_retired = v->retired;
        CHECK(platform_irq_inject(0, 0x69, IA64_IRQ_INT));
    } else if (imm == 2) {
        ivt_at = hal_counter();
        ivt_retired = v->retired;
        vcpu_request_exit(v, VCPU_EXIT_DEBUG);
    }
    return true;
}
static void latency_bundle(uint64_t ip, unsigned t, uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t low = t | (a << 5) | (b << 46), high = (b >> 18) | (c << 23);
    uint8_t bytes[16];
    for (unsigned i = 0; i < 8; ++i) {
        bytes[i] = low >> (8 * i);
        bytes[i + 8] = high >> (8 * i);
    }
    CHECK(gm_write(ip, bytes, sizeof(bytes)));
}
static void latency(void)
{
    const uint64_t base = 0x100000, iva = 0x108000, nop = UINT64_C(1) << 27;
    static uint8_t ram[65536];
    struct gm_region r = {base, 65536, GM_RAM, "latency", ram, NULL, NULL};
    CHECK(gm_add_region(&r));
    latency_bundle(base, 0x11, UINT64_C(1) << 6, nop, UINT64_C(4) << 37);
    latency_bundle(iva + 0x3000, 1, UINT64_C(2) << 6, nop, nop);
    engine_set_break_hook(latency_hook);
    static const char *names[] = {"interp", "tier0", "lockstep"};
    for (unsigned engine = 0; engine < 3; ++engine)
        for (unsigned before = 0; before < 2; ++before) {
            struct vcpu *v = cpus[0];
            struct ia64_entry_regs entry = {.ip = base, .psr = IA64_PSR_I | IA64_PSR_IC};
            vcpu_reset(v, &entry);
            v->cfg.engine = engine;
            v->s.cr[CR_IVA] = iva;
            CHECK(platform_irq_update_lid(v, 0));
            injected_at = ivt_at = 0;
            suppress_kick = before;
            uint64_t budget = 100000;
            for (unsigned slices = 0; slices < 4 && !ivt_at; ++slices) {
                platform_irq_poll(v);
                struct vcpu_exit out = engine_run(v, budget);
                CHECK(out.reason == VCPU_EXIT_BUDGET || (out.reason == VCPU_EXIT_DEBUG && ivt_at));
            }
            CHECK(ivt_at && ivt_at >= injected_at);
            uint64_t instructions = ivt_retired - injected_retired;
            CHECK(before ? instructions >= budget - 1 : instructions <= 32);
            printf("latency %s %s: %llu instructions, %llu ns injection-to-IVT\n", names[engine],
                   before ? "before" : "after", (unsigned long long)instructions,
                   (unsigned long long)((ivt_at - injected_at) * 1000000000 / hal_counter_hz()));
        }
    suppress_kick = false;
    engine_set_break_hook(NULL);
}
int main(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        /* Reset LIDs follow the platform convention (id = index, eid = 0);
         * vCPU 2 then takes eid 1 the way guest software would, by writing
         * cr.lid, so routing by eid is still exercised. */
        struct vcpu_config cfg = {.index = i, .itc_icount = true};
        cpus[i] = vcpu_create(&cfg);
        CHECK(cpus[i]);
        uint64_t lid = ((uint64_t)i << 24) | (i == 2 ? 1u << 16 : 0);
        cpus[i]->s.cr[CR_LID] = lid;
        CHECK(platform_irq_bind(cpus[i], i, lid, i + 2));
    }
    lsapic();
    pib();
    iosapic();
    translation();
    mediation();
    remapping();
    intx();
    isa_overrides();
    isa();
    concurrent();
    latency();
    for (unsigned i = 0; i < 3; ++i) {
        platform_irq_unbind(i);
        vcpu_destroy(cpus[i]);
    }
    printf("interrupts: %u checks passed (LSAPIC/PIB/IOSAPIC/MSI/MSI-X/epoch/relocation)\n",
           checks);
    return 0;
}
