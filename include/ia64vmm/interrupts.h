/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_INTERRUPTS_H
#define IA64VMM_INTERRUPTS_H
#include "ia64vmm/engine.h"
#include "ia64vmm/guest_mem.h"

#define VMM_IRQ_MAX_CPUS 256u
#define VMM_IRQ_FIRST_VECTOR 0x40u
#define VMM_IRQ_LAST_VECTOR 0xdfu
#define IA64_PIB_INTA 0x1e0000u
#define IA64_PIB_XTP 0x1e0008u
#define IA64_IOSAPIC_MAX_PINS 256u
#define IA64_IOSAPIC_MASK (1u << 16)
#define IA64_IOSAPIC_LEVEL (1u << 15)
#define IA64_IOSAPIC_REMOTE_IRR (1u << 14)
enum ia64_irq_mode {
    IA64_IRQ_INT = 0,
    IA64_IRQ_PMI = 2,
    IA64_IRQ_NMI = 4,
    IA64_IRQ_INIT = 5,
    IA64_IRQ_EXTINT = 7
};
struct ia64_irq_message {
    unsigned cpu, vector, mode;
    bool level;
};
struct irq_counters {
    uint64_t host, injected, rejected, spurious;
};

/* Lifecycle and placement changes run with the affected executors stopped.
 * IRQ senders publish mailboxes without accessing mutable architectural state.
 * An executor drains immediately before engine_run, including HALT resumes. */
bool platform_irq_bind(struct vcpu *, unsigned index, uint64_t lid, unsigned host_cpu);
void platform_irq_unbind(unsigned index);
bool platform_irq_relocate(unsigned index, unsigned host_cpu);
bool platform_irq_lid(unsigned index, uint64_t lid);
bool platform_irq_update_lid(struct vcpu *, uint64_t lid);
int platform_irq_target(unsigned id, unsigned eid);
unsigned platform_irq_host_cpu(unsigned index);
bool platform_irq_inject(unsigned index, unsigned vector, unsigned mode);
void platform_irq_poll(struct vcpu *);
uint8_t platform_irq_xtp(struct vcpu *);
void platform_irq_set_xtp(struct vcpu *, uint8_t);
void platform_irq_dump(void);
/* One setup/executor writer; vectors remain owned until stopped teardown. */
unsigned platform_irq_vector_reserve(void *owner, unsigned count);
void platform_irq_vector_release(void *owner);
struct platform_host_irq_ops {
    bool (*message)(uint16_t bdf, unsigned cpu, unsigned first, unsigned count, uint64_t *address,
                    uint32_t *data);
    /* Sources are masked before revoke; retirement joins queued dispatch. */
    void (*revoke)(unsigned first, unsigned count);
    void (*retire)(const uint64_t vectors[4]);
    bool remapped;
};
void platform_irq_set_host(const struct platform_host_irq_ops *);
bool platform_irq_host_message(uint16_t bdf, unsigned cpu, unsigned first, unsigned count,
                               uint64_t *address, uint32_t *data);
void platform_irq_host_revoke(unsigned first, unsigned count);
void platform_irq_host_retire(unsigned first, unsigned count);
bool platform_irq_remapped(void);
unsigned platform_irq_index(const struct vcpu *);
void platform_irq_kick(unsigned index);
/* Opt-in irq_latency=<vector> diagnostics: publication to external IVT
 * entry, measured in HAL counter ticks. Shadows never contribute samples. */
void platform_irq_measure_cmdline(const char *);
void platform_irq_ivt(struct vcpu *, unsigned vector);
/* Owned by the LSAPIC, used only by the executing CPU. */
bool vcpu_accept_interrupt(struct vcpu *, unsigned vector);
/* INIT, or PMI with vector 0-15, for PALE_INIT/PALE_PMI.
 * False for another vector; false with an UNIMPLEMENTED exit request
 * when the vCPU has no such entrypoint. */
bool vcpu_accept_pal_interrupt(struct vcpu *, unsigned mode, unsigned vector);
uint64_t vcpu_interrupt_lid(const struct vcpu *);
enum vcpu_local_irq {
    VCPU_LOCAL_ITV,
    VCPU_LOCAL_PMV,
    VCPU_LOCAL_CMCV,
    VCPU_LOCAL_LINT0,
    VCPU_LOCAL_LINT1
};
bool vcpu_raise_local_interrupt(struct vcpu *, enum vcpu_local_irq);

extern const struct gm_mmio_ops platform_pib_ops;
struct ia64_iosapic {
    unsigned lock; /* Executor-only lock for MMIO and input delivery. */
    uint32_t select, id, base, count;
    uint64_t rte[IA64_IOSAPIC_MAX_PINS];
    bool input[IA64_IOSAPIC_MAX_PINS], edge[IA64_IOSAPIC_MAX_PINS];
    uint64_t rejected;
};
bool platform_iosapic_init(struct ia64_iosapic *, unsigned base, unsigned count);
/* Input is the electrical pin level; polarity converts it to assertion. */
bool platform_iosapic_set_pin(struct ia64_iosapic *, unsigned gsi, bool high);
extern const struct gm_mmio_ops platform_iosapic_ops;

struct pci_host_ops;
struct pci_device;
struct pci_bar;
bool platform_msi_decode(uint64_t address, uint32_t data, struct ia64_irq_message *);
bool platform_msi_prepare(struct pci_device *, const struct pci_host_ops *, void *);
void platform_msi_config_changed(struct pci_device *);
void platform_msi_sync_pending(struct pci_device *);
/* Splits a BAR into direct regions and mediated table/PBA pages. */
const struct gm_mmio_ops *platform_msi_bar_region(struct pci_device *, struct pci_bar *,
                                                  uint64_t at, uint64_t *end, void **opaque);
void platform_msi_retarget(unsigned index);
bool platform_msi_interrupt(unsigned host_vector);
void platform_msi_cleanup(struct pci_device *);
/* Restart, management CPU with every executor
 * stopped: routes and host vectors released, shadows and host MSI-X entry
 * masks as after preparation. The state stays allocated: the pci-msix
 * regions point at it for the whole run. */
void platform_msi_restart(struct pci_device *);
void platform_msi_dump(void);

/* Host _PRT facts and level IOAPIC backend; pins are PCI's 1..4. */
struct platform_intx_ops {
    bool (*resolve)(void *, uint16_t bdf, unsigned pin, uint32_t *gsi, bool *low, bool *level);
    bool (*route)(void *, uint32_t gsi, unsigned cpu, unsigned vector, bool low);
    void (*mask)(void *, uint32_t gsi, bool masked);
    /* Atomically test the IRQ mailbox and unmask with interrupts disabled
     * on the line's host destination. Optional for serialized test backends. */
    void (*unmask_if_idle)(void *, uint32_t gsi, const unsigned *waiting);
    /* Optional: program an ISA IRQ's host GSI (MADT
     * override applied) as a masked edge input; false when the line has
     * no IOAPIC or a level override, which this edge path cannot carry. */
    bool (*route_isa)(void *, unsigned isa_irq, unsigned cpu, unsigned vector, uint32_t *gsi);
};
void platform_intx_set_host(const struct platform_intx_ops *, void *);
bool platform_intx_prepare(struct pci_device *, const struct pci_host_ops *, void *,
                           struct ia64_iosapic *);
void platform_intx_config_changed(struct pci_device *);
bool platform_intx_interrupt(unsigned vector);
void platform_intx_poll(struct vcpu *);
void platform_intx_iosapic_changed(struct ia64_iosapic *, unsigned gsi);
void platform_intx_iosapic_eoi(struct ia64_iosapic *, unsigned gsi);
void platform_intx_local_eoi(struct vcpu *, unsigned vector);
void platform_intx_cleanup(void);
/* Restart, after the IOSAPIC's re-initialization: host lines masked, line
 * and ISA state cleared, every PCI line's electrical level high again and
 * each function's command copy as after preparation. Vectors stay. */
void platform_intx_restart(void);
/* ISA IRQ of a guest-owned legacy decode (i8042 1/12, compatibility
 * IDE 14/15) on the identity guest GSI; storage lines bump the epoch. */
bool platform_intx_prepare_isa(unsigned isa_irq, struct ia64_iosapic *, bool storage);
struct ia64vmm_handoff;
bool platform_intx_handoff(struct ia64vmm_handoff *);
#endif
