/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/storage.h"
#include "platform/ahci.h"
#include "platform/platform.h"
#include "ia64vmm/pci.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"

static struct platform_block backing[PLATFORM_AHCI_MAX_PORTS];
static unsigned backing_count;
bool platform_storage_configured(void)
{
    return backing_count != 0;
}
bool platform_storage_attach(const struct platform_block *block)
{
    if (!block) {
        backing_count = 0;
        return true;
    }
    if (backing_count == PLATFORM_AHCI_MAX_PORTS || !block->sectors ||
        block->sectors > (UINT64_C(1) << 48) || !block->read ||
        (block->sector_bytes && block->sector_bytes != 512 && block->sector_bytes != 2048 &&
         block->sector_bytes != 4096) ||
        (block->sector_bytes == 2048 &&
         (!block->read_only || block->sectors > (UINT64_C(1) << 32))) ||
        (!block->read_only && (!block->write || !block->flush)))
        return false;
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *p = platform_pci_device(i);
        if (p->bdf == PLATFORM_STORAGE_BDF)
            return false;
        for (unsigned j = 0; j < PCI_BAR_COUNT; ++j) {
            const struct pci_bar *bar = p->bars + j;
            if (!bar->io && bar->size && bar->base < PLATFORM_STORAGE_BASE + PLATFORM_AHCI_SIZE &&
                PLATFORM_STORAGE_BASE < bar->base + bar->size)
                return false;
        }
    }
    backing[backing_count++] = *block;
    return true;
}
static void interrupt(void *context, bool asserted)
{
    struct platform_state *s = context;
    platform_iosapic_set_pin(&s->iosapic, PLATFORM_STORAGE_GSI, !asserted);
}
static void notify(void *context)
{
    (void)context;
    hal_cpu_request(0, HAL_CPU_WAKE);
}
bool platform_storage_init(struct platform_state *s)
{
    if (!platform_storage_configured())
        return true;
    /* PCI mapping has assigned guest GSIs; firmware's original IRQ bytes
     * do not describe these emulated IOSAPIC inputs. */
    for (unsigned i = 0; i < platform_pci_count(); ++i) {
        const struct pci_device *p = platform_pci_device(i);
        if (p->owner == PCI_OWNER_GUEST && p->irq_pin && p->irq_line == PLATFORM_STORAGE_GSI) {
            LOGE("guest storage GSI=%u conflicts with PCI %04x INTx", PLATFORM_STORAGE_GSI, p->bdf);
            return false;
        }
    }
    s->iosapic.input[PLATFORM_STORAGE_GSI] = true;
    if (!platform_ahci_init(backing, backing_count, PLATFORM_STORAGE_BASE, PLATFORM_STORAGE_GSI,
                            interrupt, s, notify, NULL))
        return false;
    const struct gm_region region = {PLATFORM_STORAGE_BASE,
                                     PLATFORM_AHCI_SIZE,
                                     GM_EMULATED,
                                     "guest-storage",
                                     NULL,
                                     &platform_ahci_ops,
                                     NULL};
    if (!gm_add_region(&region)) {
        platform_ahci_cleanup();
        return false;
    }
    LOGI("guest storage: AHCI 00:1f.0 BAR=%llx GSI=%u ports=%u",
         (unsigned long long)PLATFORM_STORAGE_BASE, PLATFORM_STORAGE_GSI, backing_count);
    for (unsigned i = 0; i < backing_count; ++i)
        LOGI("guest storage port=%u sectors=%llu sector_bytes=%u readonly=%u", i,
             (unsigned long long)backing[i].sectors,
             backing[i].sector_bytes ? backing[i].sector_bytes : 512, backing[i].read_only);
    return true;
}
