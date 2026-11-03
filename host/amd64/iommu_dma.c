/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/iommu_dma.h"

static int device(const struct pci_device *const *d, unsigned n, uint16_t bdf)
{
    for (unsigned i = 0; i < n; ++i)
        if (d[i]->bdf == bdf)
            return (int)i;
    return -1;
}
static bool descendant(const int *parents, unsigned child, unsigned ancestor)
{
    for (int at = (int)child; at >= 0; at = parents[at])
        if ((unsigned)at == ancestor)
            return true;
    return false;
}
static bool acs_enable(const struct amd64_iommu_dma_ops *ops, void *context, uint16_t bdf,
                       unsigned at)
{
    unsigned cap = ops->config(context, bdf, at + 4, 2);
    unsigned control = ops->config(context, bdf, at + 6, 2);
    if (cap == UINT16_MAX || control == UINT16_MAX)
        return false;
    /* PCIe Base 2.1 §6.12.3 Table 6-9: Egress Control can override
     * Request Redirect with a permissive vector bit. Disable that path
     * and Direct Translated P2P, so RR applies independently of ARI
     * Function Group membership or firmware's Egress Control Vector. */
    unsigned wanted = (control | (cap & 0x1f)) & ~(cap & 0x60);
    if (wanted != control) {
        if (!ops->config_write)
            return false;
        ops->config_write(context, bdf, at + 6, wanted, 2);
        control = ops->config(context, bdf, at + 6, 2);
    }
    /* Within a present ACS capability, absent bits are hardwired or inapplicable. */
    return control != UINT16_MAX && (control & cap & 0x7f) == (cap & 0x1f);
}
/* Reject source-ID replacement and firmware-enabled translated requests. */
const char *amd64_iommu_topology(const struct pci_device *const *d, unsigned n,
                                 const struct amd64_iommu_dma_ops *ops, void *context)
{
    if (n > PCI_MAX_DEVICES)
        return "too many PCI functions";
    bool isolated[PCI_MAX_DEVICES] = {0}, path_isolated[PCI_MAX_DEVICES] = {0};
    bool root_port[PCI_MAX_DEVICES] = {0};
    bool ari_bus[256] = {0}, acs_present[PCI_MAX_DEVICES] = {0};
    bool acs_isolated[PCI_MAX_DEVICES] = {0};
    unsigned pcie_type[PCI_MAX_DEVICES], groups[PCI_MAX_DEVICES];
    unsigned owners[PCI_MAX_DEVICES] = {0};
    int parents[PCI_MAX_DEVICES];
    for (unsigned i = 0; i < n; ++i)
        if (!d[i] || d[i]->header > 1 ||
            (d[i]->owner != PCI_OWNER_GUEST && d[i]->owner != PCI_OWNER_VMM) ||
            device(d, i, d[i]->bdf) >= 0)
            return "unsupported/duplicate PCI function";
    for (unsigned i = 0; i < n; ++i) {
        const struct pci_device *p = d[i];
        unsigned pcie = 255;
        bool pcix2 = false;
        parents[i] = p->parent == 0xffff ? -1 : device(d, n, p->parent);
        if (p->parent != 0xffff) {
            int parent = device(d, n, p->parent);
            if (parent < 0 || d[parent]->header != 1 || (p->bdf >> 8) < d[parent]->secondary ||
                (p->bdf >> 8) > d[parent]->subordinate)
                return "invalid PCI ancestry";
            unsigned current = i;
            for (unsigned depth = 0;; ++depth) {
                int next = device(d, n, d[current]->parent);
                if (next < 0)
                    break;
                if (depth >= n || (unsigned)next == i)
                    return "cyclic PCI ancestry";
                current = (unsigned)next;
            }
        }
        if (ops->config(context, p->bdf, 6, 2) & 0x10) {
            bool seen[256] = {0};
            unsigned at = ops->config(context, p->bdf, 0x34, 1) & ~3u;
            while (at) {
                if (at < 0x40 || at > 0xfc || seen[at])
                    return "malformed PCI capabilities";
                seen[at] = true;
                unsigned id = ops->config(context, p->bdf, at, 1);
                if (id == 7) {
                    if (at > 0xf8)
                        return "malformed PCI-X capability";
                    pcix2 = (ops->config(context, p->bdf, at + 4, 4) & UINT32_C(0xc0000000)) != 0;
                }
                if (id == 0x10) {
                    if (pcie != 255 || at > 0xf4)
                        return "malformed PCIe capability";
                    unsigned capability = ops->config(context, p->bdf, at + 2, 2);
                    pcie = (capability >> 4) & 15;
                    if (ops->config(context, p->bdf, at + 8, 2) & (1u << 9))
                        return "PCIe phantom functions enabled";
                    if ((pcie == 4 || pcie == 6) && (capability & 15) >= 2) {
                        if (at > 0xd4)
                            return "malformed PCIe ARI forwarding capability";
                        if (ops->config(context, p->bdf, at + 0x28, 2) & 0x20) {
                            if (p->header != 1 || !p->secondary || p->secondary <= (p->bdf >> 8) ||
                                p->secondary > p->subordinate ||
                                !(ops->config(context, p->bdf, at + 0x24, 4) & 0x20))
                                return "invalid PCIe ARI forwarding configuration";
                            ari_bus[p->secondary] = true;
                        }
                    }
                }
                at = ops->config(context, p->bdf, at + 1, 1) & ~3u;
            }
        }
        pcie_type[i] = pcie;
        if (p->header == 1) {
            if ((pcie != 4 && pcie != 5 && pcie != 6) || !p->secondary ||
                p->secondary > p->subordinate)
                return "PCI bridge requester aliases unsupported";
            root_port[i] = pcie == 4;
        }
        /* Guest config mediation hides all extended capabilities. Refuse
         * firmware-enabled translated requests; this backend never enables ATS. */
        /* Other functions, including host bridges, may have private registers here. */
        if (pcie == 255 && !pcix2)
            continue;
        unsigned at = 0x100, acs = 0;
        bool extended_seen[1024] = {0};
        for (unsigned steps = 0; at; ++steps) {
            if (steps >= 960 || at < 0x100 || at > 0xffc)
                return "malformed PCI extended capabilities";
            if (extended_seen[at / 4])
                return "cyclic PCI extended capabilities";
            extended_seen[at / 4] = true;
            uint32_t h = ops->config(context, p->bdf, at, 4);
            if (!h)
                break;
            if (h == UINT32_MAX)
                return "PCIe extended configuration unavailable";
            unsigned id = h & 0xffff;
            if (id == 0x0d) {
                if (acs || at > 0xff8 || !(h & 0xf0000))
                    return "malformed PCI ACS capability";
                acs = at;
            }
            unsigned control = id == 0x0f || id == 0x1b ? 6 : id == 0x13 ? 4 : id == 0x10 ? 8 : 0;
            if (control) {
                if (at + control + 2 > 4096)
                    return "malformed PCI extended capabilities";
                if (ops->config(context, p->bdf, at + control, 2) & (id == 0x0f ? 0x8000 : 1))
                    return "firmware ATS/PRI/PASID/SR-IOV enabled";
            }
            /* PCIe Base 2.1 §7.9: software must mask the reserved low two bits. */
            at = (h >> 20) & 0xffc;
        }
        acs_present[i] = acs != 0;
        if (acs && (pcie == 0 || pcie == 1 || pcie == 4 || pcie == 5 || pcie == 6 || pcie == 9))
            acs_isolated[i] = acs_enable(ops, context, p->bdf, acs);
    }
    /* ARI mode belongs only to the immediately downstream bus. Group all
     * eight low RID bits there, including when Function 0.MF is clear.
     * Determine this after every port has been inspected: inventory order
     * is not a topology guarantee for callers of this pure check. */
    for (unsigned i = 0; i < n; ++i)
        groups[i] = d[i]->bdf & (ari_bus[d[i]->bdf >> 8] ? 0xff00u : 0xfff8u);
    for (unsigned i = 0; i < n; ++i) {
        unsigned pcie = pcie_type[i];
        bool multifunction = ari_bus[d[i]->bdf >> 8];
        int first = device(d, n, groups[i]);
        if (first >= 0)
            multifunction |= (ops->config(context, d[first]->bdf, 0x0e, 1) & 0x80) != 0;
        for (unsigned j = 0; j < n; ++j)
            if (i != j && groups[i] == groups[j])
                multifunction = true;
        if (pcie == 0 || pcie == 1 || pcie == 5 || pcie == 9)
            isolated[i] = !multifunction;
        if (acs_present[i])
            isolated[i] = acs_isolated[i];
    }
    /* Bridge ownership follows its DMA requesters, not its mediated config space. */
    unsigned all_owners = 0;
    for (unsigned i = 0; i < n; ++i) {
        int at = (int)i;
        while (at >= 0 && isolated[at])
            at = parents[at];
        path_isolated[i] = at < 0;
        if (!d[i]->header) {
            owners[i] = 1u << (d[i]->owner == PCI_OWNER_VMM);
            all_owners |= owners[i];
        } else
            for (unsigned j = 0; j < n; ++j)
                if (!d[j]->header && descendant(parents, j, i))
                    owners[i] |= 1u << (d[j]->owner == PCI_OWNER_VMM);
    }
    for (unsigned i = 0; i < n; ++i) {
        if (all_owners == 3 && root_port[i] && owners[i] && !isolated[i])
            return "PCIe root port lacks ACS isolation";
        for (unsigned j = 0; j < i; ++j)
            if (groups[i] == groups[j] && (owners[i] | owners[j]) == 3 && !isolated[i] &&
                !isolated[j])
                return "guest/VMM share a multifunction PCI device; ACS isolation unavailable";
        /* Every branch of a shared hierarchy needs an isolated route to the root. */
        if (d[i]->header && owners[i] == 3)
            for (unsigned j = 0; j < n; ++j)
                if (!d[j]->header && descendant(parents, j, i) &&
                    (parents[j] < 0 || !path_isolated[parents[j]]))
                    return "guest/VMM share downstream PCI hierarchy; ACS isolation unavailable";
    }
    return NULL;
}
bool amd64_iommu_covered(const struct boot_info *bi, uint64_t start, uint64_t end,
                         enum boot_mem_type type)
{
    while (start < end) {
        uint64_t next = start;
        for (unsigned i = 0; i < bi->mem_range_count; ++i) {
            const struct boot_mem_range *m = &bi->mem[i];
            if (m->type == type && start >= m->base && start - m->base < m->size)
                next = m->base + m->size;
        }
        if (next == start)
            return false;
        start = next;
    }
    return true;
}

const char *amd64_iommu_memory(const struct boot_info *bi)
{
    for (unsigned i = 0; i < bi->mem_range_count; ++i) {
        const struct boot_mem_range *m = &bi->mem[i];
        if (m->size > UINT64_MAX - m->base)
            return "invalid DMA memory map";
        for (unsigned j = 0; j < i; ++j) {
            const struct boot_mem_range *old = &bi->mem[j];
            if (m->size && old->size && m->base < old->base + old->size &&
                old->base < m->base + m->size)
                return "overlapping DMA memory ownership";
        }
    }
    return NULL;
}
