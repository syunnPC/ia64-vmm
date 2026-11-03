/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel VT-d architecture rev. 4.1, §§3.12, 6.2.2, 8.3/8.4, 9.1/9.3/9.8,
 * 11.4.4–11.4.6. Fixtures encode architectural formats independently. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "host/amd64/vtd.h"

#define POOL_BASE UINT64_C(0x10000000)
#define POOL_PAGES 4096u
static _Alignas(4096) uint8_t pool[POOL_PAGES][4096];
struct fixture {
    unsigned pages, flushes, writes, stage[2], fail_stage;
    uint64_t cap[2], ecap[2], root[2], context[2], iotlb[2];
    uint32_t status[2], fault_control[2], pmen[2], version;
    unsigned pcie[PCI_MAX_DEVICES], ats, phantom, bad_granularity, sriov;
    bool ari;
    bool bad_physical;
    unsigned backwards;
};
static struct fixture f;
static struct boot_info bi;
static struct pci_device devices[4];
static const struct pci_device *list[4];
static uint8_t dmar[512];
static unsigned length;
static struct amd64_vtd state;
static void put(uint8_t *p, unsigned n, uint64_t value)
{
    for (unsigned i = 0; i < n; ++i)
        p[i] = value >> (8 * i);
}
static void seal(void)
{
    put(dmar + 4, 4, length);
    dmar[9] = 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < length; ++i)
        sum += dmar[i];
    dmar[9] = -sum;
}
static void *allocate(void *p, unsigned pages)
{
    struct fixture *r = p;
    assert(pages == 1);
    return r->pages < POOL_PAGES ? pool[r->pages++] : NULL;
}
static bool physical(void *p, const void *v, uint64_t *pa)
{
    struct fixture *r = p;
    *pa = r->bad_physical ? UINT64_C(1) << 48 : POOL_BASE + (uintptr_t)v - (uintptr_t)pool;
    return true;
}
static void *pointer(void *p, uint64_t pa)
{
    (void)p;
    assert(pa >= POOL_BASE && pa - POOL_BASE < sizeof(pool));
    return (uint8_t *)pool + pa - POOL_BASE;
}
static void flush(void *p, const void *v, size_t n)
{
    (void)v;
    assert(n == 4096);
    ++((struct fixture *)p)->flushes;
}
static uint32_t config(void *p, uint16_t bdf, unsigned off, unsigned size)
{
    (void)size;
    struct fixture *r = p;
    unsigned i = 0;
    while (i < 4 && devices[i].bdf != bdf)
        ++i;
    assert(i < 4);
    if (off == 6)
        return r->pcie[i] != 255 ? 0x10 : 0;
    if (off == 0x34)
        return 0x40;
    if (off == 0x40)
        return 0x10;
    if (off == 0x41)
        return 0;
    if (off == 0x42)
        return (r->pcie[i] << 4) | 2;
    if (off == 0x48)
        return r->phantom;
    if (off == 0x68)
        return r->ari ? 0x20 : 0;
    if (off == 0x64)
        return r->ari ? 0x20 : 0;
    if (r->backwards) {
        if (off == 0x100)
            return 0x20010001;
        if (off == 0x200)
            return 0x1801000e;
        if (off == 0x180)
            return r->backwards == 2 ? 0x20010003 : 0x10003;
    }
    if (off == 0x100)
        return r->sriov ? 0x00010010 : r->ats ? 0x0001000f : 0;
    if (off == 0x106)
        return r->ats == 2 ? 0x8000 : 0;
    if (off == 0x108)
        return r->sriov == 2 ? 1 : 0;
    return 0;
}
static uint64_t read_reg(void *p, unsigned u, unsigned off, unsigned width)
{
    (void)width;
    struct fixture *r = p;
    assert(u < 2);
    if (off == 0)
        return r->version;
    if (off == 8)
        return r->cap[u];
    if (off == 0x10)
        return r->ecap[u];
    if (off == 0x1c)
        return r->status[u];
    if (off == 0x38)
        return r->fault_control[u];
    if (off == 0x64)
        return r->pmen[u];
    if (off == 0x28)
        return r->context[u];
    assert(off == ((r->ecap[u] >> 8) & 1023) * 16 + 8);
    return r->iotlb[u];
}
static void write_reg(void *p, unsigned u, unsigned off, uint64_t value, unsigned width)
{
    struct fixture *r = p;
    ++r->writes;
    if (off == 0x38) {
        assert(width == 4);
        r->fault_control[u] = value;
        return;
    }
    if (off == 0x20) {
        assert(width == 8);
        assert(!r->stage[u]);
        r->root[u] = value;
        return;
    }
    if (off == 0x18) {
        assert(width == 4);
        if (value & (1u << 30)) {
            assert(r->root[u] && r->stage[u] == 0);
            r->stage[u] = 1;
            if (r->fail_stage != 1)
                r->status[u] |= 1u << 30;
        } else if (value & (1u << 31)) {
            assert(r->stage[u] == 3);
            r->stage[u] = 4;
            if (r->fail_stage != 4)
                r->status[u] |= 1u << 31;
        } else {
            assert(!r->stage[u]);
            if (r->fail_stage != 5)
                r->status[u] &= ~(1u << 26);
        }
        return;
    }
    assert(width == 8);
    if (off == 0x28) {
        assert(r->stage[u] == 1 && (value >> 61) == 5);
        r->stage[u] = 2;
        r->context[u] = r->fail_stage == 2         ? value
                        : (r->bad_granularity & 1) ? 0
                                                   : UINT64_C(1) << 59;
    } else {
        assert(off == ((r->ecap[u] >> 8) & 1023) * 16 + 8);
        assert(r->stage[u] == 2 && (value >> 60) == 9);
        r->stage[u] = 3;
        assert((value & (UINT64_C(3) << 48)) == UINT64_C(3) << 48);
        r->iotlb[u] = r->fail_stage == 3 ? value : (r->bad_granularity & 2) ? 0 : UINT64_C(1) << 57;
    }
}
static const struct amd64_iommu_dma_ops ops = {allocate, physical, pointer,   flush,
                                               config,   read_reg, write_reg, NULL};
static void reset(void)
{
    memset(&f, 0, sizeof(f));
    memset(&bi, 0, sizeof(bi));
    memset(devices, 0, sizeof(devices));
    memset(dmar, 0, sizeof(dmar));
    for (unsigned i = 0; i < PCI_MAX_DEVICES; ++i)
        f.pcie[i] = 255;
    f.version = 0x10;
    f.cap[0] = f.cap[1] = (UINT64_C(3) << 54) | (UINT64_C(1) << 34) | (UINT64_C(47) << 16) | 0x600;
    f.ecap[0] = f.ecap[1] = 0x1001; /* coherent, IOTLB block at 100h */
    memcpy(dmar, "DMAR", 4);
    dmar[36] = 47;
    length = 64;
    put(dmar + 50, 2, 16);
    dmar[52] = 1;
    put(dmar + 56, 8, 0xfed90000);
    bi.mem_range_count = 4;
    bi.mem[0] = (struct boot_mem_range){0x1003, 0x2ff9, BOOT_MEM_USABLE, NULL};
    bi.mem[1] = (struct boot_mem_range){0x200000, 0x400000, BOOT_MEM_USABLE, NULL};
    bi.mem[2] = (struct boot_mem_range){POOL_BASE, sizeof(pool), BOOT_MEM_VMM, NULL};
    bi.mem[3] = (struct boot_mem_range){0x700000, 0x2000, BOOT_MEM_RESERVED, NULL};
    devices[0] = (struct pci_device){.bdf = 0x20, .parent = 0xffff, .owner = PCI_OWNER_GUEST};
    devices[1] = (struct pci_device){.bdf = 0x18, .parent = 0xffff, .owner = PCI_OWNER_VMM};
    for (unsigned i = 0; i < 4; ++i)
        list[i] = &devices[i];
    seal();
}
static bool build(unsigned count)
{
    return amd64_vtd_build(dmar, length, &bi, list, count, &ops, &f, &state);
}

struct pci_fixture {
    uint8_t bytes[4096];
    unsigned extended_reads;
};
static uint32_t pci_config(void *p, uint16_t bdf, unsigned off, unsigned width)
{
    struct pci_fixture *r = p;
    assert(bdf == 0x20 && (width == 1 || width == 2 || width == 4));
    assert(!(off & (width - 1)) && off + width <= sizeof(r->bytes));
    if (off >= 0x100)
        ++r->extended_reads;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= (uint32_t)r->bytes[off + i] << (8 * i);
    return value;
}
/* PCIe Base 2.1 §§7.8/7.9: capability identification and reserved pointer bits. */
static void capabilities(void)
{
    struct pci_fixture r = {0};
    struct pci_device p = {.bdf = 0x20, .parent = 0xffff, .owner = PCI_OWNER_GUEST};
    const struct pci_device *d[] = {&p};
    const struct amd64_iommu_dma_ops cfg_ops = {.config = pci_config};

    /* Conventional functions may mirror config or expose private registers. */
    put(r.bytes, 4, 0x12348086);
    put(r.bytes + 0x100, 4, 0x12348086);
    assert(!amd64_iommu_topology(d, 1, &cfg_ops, &r) && !r.extended_reads);
    p.class_code = 0x060000;
    put(r.bytes + 0x100, 4, 0x04201234);
    assert(!amd64_iommu_topology(d, 1, &cfg_ops, &r) && !r.extended_reads);

    r.bytes[6] = 0x10;
    r.bytes[0x34] = 0x40;
    r.bytes[0x40] = 7;
    assert(!amd64_iommu_topology(d, 1, &cfg_ops, &r) && !r.extended_reads);
    /* PCI-X 266/533 MHz devices implement extended configuration space. */
    for (unsigned bit = 30; bit <= 31; ++bit) {
        put(r.bytes + 0x44, 4, UINT32_C(1) << bit);
        assert(amd64_iommu_topology(d, 1, &cfg_ops, &r));
    }
    put(r.bytes + 0x44, 4, 0);
    r.bytes[0x40] = 0x10;
    r.bytes[0x42] = 2;

    static const struct {
        unsigned id, control, enable;
    } translated[] = {
        {0x0f, 6, 0x8000},
        {0x13, 4, 1},
        {0x1b, 6, 1},
        {0x10, 8, 1},
    };
    for (unsigned flags = 0; flags < 4; ++flags) {
        put(r.bytes + 0x100, 4, ((0x180u | flags) << 20) | 0x10003);
        for (unsigned i = 0; i < sizeof(translated) / sizeof(translated[0]); ++i) {
            put(r.bytes + 0x180, 4, (flags << 20) | 0x10000 | translated[i].id);
            assert(!amd64_iommu_topology(d, 1, &cfg_ops, &r));
            put(r.bytes + 0x180 + translated[i].control, 2, translated[i].enable);
            assert(!strcmp(amd64_iommu_topology(d, 1, &cfg_ops, &r),
                           "firmware ATS/PRI/PASID/SR-IOV enabled"));
            put(r.bytes + 0x180 + translated[i].control, 2, 0);
        }
    }
    put(r.bytes + 0x100, 4, 0x08010003);
    assert(
        !strcmp(amd64_iommu_topology(d, 1, &cfg_ops, &r), "malformed PCI extended capabilities"));
    put(r.bytes + 0x100, 4, 0x18010003);
    put(r.bytes + 0x180, 4, 0x10010003);
    assert(!strcmp(amd64_iommu_topology(d, 1, &cfg_ops, &r), "cyclic PCI extended capabilities"));
    put(r.bytes + 0x180, 4, UINT32_MAX);
    assert(!strcmp(amd64_iommu_topology(d, 1, &cfg_ops, &r),
                   "PCIe extended configuration unavailable"));
    put(r.bytes + 0x100, 4, 0xffc10003);
    put(r.bytes + 0xffc, 4, 0x1000f);
    assert(
        !strcmp(amd64_iommu_topology(d, 1, &cfg_ops, &r), "malformed PCI extended capabilities"));
}

static struct {
    struct pci_device devices[6];
    uint8_t config[6][4096];
    unsigned count, writes;
    bool stuck;
} topology;
static unsigned topology_index(uint16_t bdf)
{
    for (unsigned i = 0; i < topology.count; ++i)
        if (topology.devices[i].bdf == bdf)
            return i;
    assert(false);
    return 0;
}
static uint32_t topology_read(void *context, uint16_t bdf, unsigned off, unsigned width)
{
    (void)context;
    assert((width == 1 || width == 2 || width == 4) && !(off & (width - 1)) && off + width <= 4096);
    const uint8_t *p = topology.config[topology_index(bdf)] + off;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= (uint32_t)p[i] << (8 * i);
    return value;
}
static void topology_write(void *context, uint16_t bdf, unsigned off, uint32_t value,
                           unsigned width)
{
    (void)context;
    assert(off == 0x106 && width == 2);
    ++topology.writes;
    if (!topology.stuck)
        put(topology.config[topology_index(bdf)] + off, width, value);
}
static void topology_device(unsigned i, uint16_t bdf, uint16_t parent, unsigned type,
                            enum pci_owner owner)
{
    bool bridge = type == 4 || type == 5 || type == 6;
    topology.devices[i] = (struct pci_device){.bdf = bdf,
                                              .parent = parent,
                                              .owner = owner,
                                              .header = bridge,
                                              .secondary = bridge ? (bdf >> 8) + 1 : 0,
                                              .subordinate = bridge ? 4 : 0};
    if (type != 255) {
        topology.config[i][6] = 0x10;
        topology.config[i][0x34] = 0x40;
        topology.config[i][0x40] = 0x10;
        topology.config[i][0x42] = (type << 4) | 2;
    }
    if (topology.count <= i)
        topology.count = i + 1;
}
static void topology_acs(unsigned i, unsigned cap, unsigned control)
{
    put(topology.config[i] + 0x100, 4, 0x1000d);
    put(topology.config[i] + 0x104, 2, cap);
    put(topology.config[i] + 0x106, 2, control);
}
static const char *topology_check(bool writable)
{
    const struct pci_device *d[6];
    for (unsigned i = 0; i < topology.count; ++i)
        d[i] = &topology.devices[i];
    const struct amd64_iommu_dma_ops cfg_ops = {
        .config = topology_read,
        .config_write = writable ? topology_write : NULL,
    };
    return amd64_iommu_topology(d, topology.count, &cfg_ops, NULL);
}
/* ARI changes the grouping of the low eight RID bits, not the RID itself.
 * The Header Type multifunction bit need not be set on an ARI Function 0. */
static void ari_isolation(void)
{
    memset(&topology, 0, sizeof(topology));
    topology_device(0, 0x08, 0xffff, 4, PCI_OWNER_GUEST);
    topology_device(1, 0x100, 0x08, 0, PCI_OWNER_GUEST);
    topology_device(2, 0x189, 0x08, 0, PCI_OWNER_GUEST);
    put(topology.config[0] + 0x64, 4, 0x20); /* ARI Forwarding Supported */
    put(topology.config[0] + 0x68, 2, 0x20); /* ARI Forwarding Enable */
    assert(!topology_check(false));          /* One owner does not require endpoint ACS. */
    topology.devices[2].owner = PCI_OWNER_VMM;
    topology_acs(0, 0x1f, 0x1f);
    assert(topology_check(false)); /* Functions 0 and 137 share an ARI device. */
    topology_acs(2, 0, 0);
    assert(!topology_check(false));
    /* ARI Function Groups affect Egress Control Vectors, not RR. A
     * firmware-enabled permissive vector must not override isolation. */
    put(topology.config[1] + 0x100, 4, 0x0001000e);
    put(topology.config[1] + 0x104, 2, (137u << 8) | 2);
    put(topology.config[1] + 0x106, 2, 2);
    topology_acs(2, 0x6c, 0x60);
    assert(topology_check(false));
    assert(!topology_check(true));
    assert(topology_read(NULL, 0x189, 0x106, 2) == 0x0c);
    assert(topology_read(NULL, 0x100, 0x106, 2) == 2);
    topology_acs(2, 0x6c, 0x2c);
    topology.stuck = true;
    assert(topology_check(true));
    topology.stuck = false;
    put(topology.config[2] + 0x100, 4, 0);
    put(topology.config[0] + 0x68, 2, 0);
    assert(!topology_check(false)); /* Conventional device numbers remain distinct. */
    /* The control bit is reserved on an Upstream Port; do not treat it as
     * forwarding onto that port's secondary bus. */
    topology.config[0][0x42] = 0x52;
    put(topology.config[0] + 0x68, 2, 0x20);
    assert(!topology_check(false));
    topology.config[0][0x42] = 0x42;
    put(topology.config[0] + 0x64, 4, 0);
    assert(topology_check(false)); /* An enabled but unsupported feature is corrupt. */
}
/* PCIe ACS separates functions and redirects peer traffic through the IOMMU. */
static void acs_isolation(void)
{
    memset(&topology, 0, sizeof(topology));
    topology_device(0, 0x08, 0xffff, 255, PCI_OWNER_VMM);
    topology.devices[0].class_code = 0x060000;
    topology_device(1, 0x09, 0xffff, 4, PCI_OWNER_GUEST);
    topology_device(2, 0x100, 0x09, 0, PCI_OWNER_GUEST);
    topology_device(3, 0x101, 0x09, 0, PCI_OWNER_VMM);
    topology_acs(1, 0x5f, 0x40);
    topology_acs(3, 0, 0); /* Present ACS with no controllable peer routing. */
    assert(!topology_check(true) && topology.writes == 1);
    assert(topology_read(NULL, 0x09, 0x106, 2) == 0x1f);
    assert(!topology_check(false));
    put(topology.config[3] + 0x100, 4, 0);
    assert(topology_check(true)); /* Absent ACS is not equivalent to cap=0. */
    topology_acs(3, 0, 0);
    for (unsigned bit = 0; bit < 5; ++bit) {
        topology_acs(1, 0x5f, 0x1f & ~(1u << bit));
        assert(topology_check(false));
        assert(!topology_check(true));
    }
    topology.stuck = true;
    topology_acs(1, 0x5f, 0x40);
    assert(topology_check(true));

    /* Both switch branches need isolation, even when each has one owner. */
    memset(&topology, 0, sizeof(topology));
    topology_device(0, 0x08, 0xffff, 4, PCI_OWNER_GUEST);
    topology_device(1, 0x100, 0x08, 5, PCI_OWNER_GUEST);
    topology_device(2, 0x200, 0x100, 6, PCI_OWNER_GUEST);
    topology.devices[2].subordinate = 3;
    topology_device(3, 0x208, 0x100, 6, PCI_OWNER_GUEST);
    topology.devices[3].secondary = 4;
    topology_device(4, 0x300, 0x200, 0, PCI_OWNER_GUEST);
    topology_device(5, 0x400, 0x208, 0, PCI_OWNER_VMM);
    topology_acs(0, 0x1f, 0x1f);
    topology_acs(2, 0x1f, 0x1f);
    topology_acs(3, 0x1f, 0x1f);
    assert(!topology_check(false));
    put(topology.config[0] + 0x100, 4, 0);
    assert(topology_check(false)); /* A gap above the switch still merges domains. */
    topology_acs(0, 0x1f, 0x1f);
    put(topology.config[2] + 0x100, 4, 0);
    put(topology.config[3] + 0x100, 4, 0);
    assert(topology_check(false));
    topology_acs(2, 0x1f, 0x1f);
    topology_acs(3, 0x1f, 0x1f);
    topology.config[1][0x0e] = 0x81;
    assert(topology_check(false)); /* Multifunction upstream port needs ACS too. */
    topology_acs(1, 0, 0);
    assert(!topology_check(false));

    /* Separate root-port slots still need peer redirection across domains. */
    memset(&topology, 0, sizeof(topology));
    topology_device(0, 0x08, 0xffff, 4, PCI_OWNER_GUEST);
    topology.devices[0].subordinate = 1;
    topology_device(1, 0x10, 0xffff, 4, PCI_OWNER_GUEST);
    topology.devices[1].secondary = topology.devices[1].subordinate = 2;
    topology_device(2, 0x100, 0x08, 0, PCI_OWNER_GUEST);
    topology_device(3, 0x200, 0x10, 0, PCI_OWNER_VMM);
    assert(topology_check(false));
    topology_acs(0, 0x1f, 0x1f);
    assert(topology_check(false));
    topology_acs(1, 0x1f, 0x1f);
    assert(!topology_check(false));

    /* Internal peers in a homogeneous subtree share a domain behind its root. */
    memset(&topology, 0, sizeof(topology));
    topology_device(0, 0x08, 0xffff, 4, PCI_OWNER_GUEST);
    topology.devices[0].subordinate = 3;
    topology_device(1, 0x100, 0x08, 5, PCI_OWNER_GUEST);
    topology.devices[1].subordinate = 3;
    topology_device(2, 0x200, 0x100, 6, PCI_OWNER_GUEST);
    topology.devices[2].subordinate = 3;
    topology_device(3, 0x300, 0x200, 0, PCI_OWNER_GUEST);
    topology_device(4, 0x10, 0xffff, 4, PCI_OWNER_GUEST);
    topology.devices[4].secondary = topology.devices[4].subordinate = 4;
    topology_device(5, 0x400, 0x10, 0, PCI_OWNER_VMM);
    topology_acs(0, 0x1f, 0x1f);
    topology_acs(4, 0x1f, 0x1f);
    assert(!topology_check(false));
}
/* Independent hardware walk; verify reserved fields, permissions and DID. */
static bool walk(unsigned u, uint16_t bdf, uint64_t address, unsigned did)
{
    uint64_t *root = pointer(&f, state.units[u].root);
    assert(!root[2 * (bdf >> 8) + 1]);
    uint64_t entry = root[2 * (bdf >> 8)];
    if (!(entry & 1))
        return false;
    assert(!(entry & 0xffe));
    uint64_t *context = pointer(&f, entry & ~UINT64_C(4095));
    entry = context[2 * (bdf & 255)];
    if (!(entry & 1))
        return false;
    assert(!(entry & 0xffe));
    uint64_t high = context[2 * (bdf & 255) + 1];
    assert((high >> 8) == did);
    unsigned levels = (high & 7) + 2;
    if (address >> (12 + levels * 9))
        return false;
    uint64_t *table = pointer(&f, entry & ~UINT64_C(4095));
    while (levels) {
        unsigned shift = 12 + (--levels) * 9;
        entry = table[(address >> shift) & 511];
        if (!(entry & 3))
            return false;
        assert((entry & 3) == 3 && !(entry & 0xf7c));
        if (!levels || (entry & 128)) {
            uint64_t mask = (UINT64_C(1) << shift) - 1;
            assert(((entry & UINT64_C(0x000ffffffffff000)) | (address & mask)) == address);
            return true;
        }
        table = pointer(&f, entry & UINT64_C(0x000ffffffffff000));
    }
    return false;
}
static void mapping(void)
{
    reset();
    assert(build(2));
    assert(!f.writes && !f.flushes);
    assert(walk(0, 0x20, 0x2000, 1) && walk(0, 0x20, 0x5fffff, 1));
    assert(!walk(0, 0x20, 0x1003, 1) && !walk(0, 0x20, 0x3000, 1));
    assert(!walk(0, 0x20, POOL_BASE, 1) && !walk(0, 0x28, 0x2000, 1));
    assert(walk(0, 0x18, POOL_BASE, 2) && !walk(0, 0x18, 0x2000, 2));
    assert(amd64_vtd_enable(&state) && f.stage[0] == 4 && f.root[0] == state.units[0].root);
    reset();
    f.ecap[0] &= ~UINT64_C(1);
    assert(build(2));
    assert(f.flushes == f.pages);
    reset();
    f.cap[0] &= ~(UINT64_C(1) << 34);
    assert(build(2));
    assert(walk(0, 0x20, 0x400001, 1));
    reset();
    f.cap[0] &= ~UINT64_C(0x200);
    assert(build(2));
    assert(state.units[0].levels == 4);
    reset();
    f.cap[0] &= ~UINT64_C(0x600);
    assert(!build(2) && !f.writes);
    reset();
    f.bad_physical = true;
    assert(!build(2));
    reset();
    f.pages = POOL_PAGES;
    assert(!build(2));
    reset();
    bi.mem[1].base = UINT64_C(1) << 48;
    assert(!build(2));
    reset();
    bi.mem[1].base = POOL_BASE;
    assert(!build(2));
    reset();
    bi.mem[0].base = UINT64_MAX - 1024;
    assert(!build(2));
    reset();
    f.cap[0] &= ~(UINT64_C(63) << 16);
    assert(!build(2));
    reset();
    f.version = 0x60;
    assert(!build(2));
    reset();
    f.pcie[0] = 0;
    f.ats = 1;
    assert(build(2));
    reset();
    f.pcie[0] = 0;
    f.ats = 2;
    assert(!build(2));
    reset();
    f.pcie[0] = 0;
    f.backwards = 1;
    assert(build(2));
    reset();
    f.pcie[0] = 0;
    f.backwards = 2;
    assert(!build(2));
    reset();
    f.pcie[0] = 0;
    f.phantom = 1u << 9;
    assert(!build(2));
    reset();
    devices[0].bdf = 0x19;
    assert(!build(2)); /* one physical device split across owners */
    devices[0].owner = PCI_OWNER_VMM;
    assert(build(2));
    reset();
    f.pcie[0] = 0;
    f.sriov = 1;
    assert(build(2));
    f.sriov = 2;
    assert(!build(2));
    reset();
    f.pcie[0] = 4;
    devices[0].header = 1;
    devices[0].secondary = devices[0].subordinate = 1;
    f.ari = true;
    assert(build(2)); /* Enabled forwarding preserves the same requester IDs. */
}
static void scope(unsigned at, unsigned type, uint8_t bus, uint8_t device)
{
    dmar[at] = type;
    dmar[at + 1] = 8;
    dmar[at + 5] = bus;
    dmar[at + 6] = device;
}
static void scopes(void)
{
    reset();
    dmar[52] = 0;
    put(dmar + 50, 2, 24);
    scope(64, 1, 0, 4);
    length = 88;
    put(dmar + 74, 2, 16);
    dmar[76] = 1;
    put(dmar + 80, 8, 0xfeda0000);
    seal();
    assert(build(2) && state.count == 2);
    assert(walk(0, 0x20, 0x2000, 1) && !walk(0, 0x18, POOL_BASE, 2));
    assert(walk(1, 0x18, POOL_BASE, 2) && !walk(1, 0x20, 0x2000, 1));
    f.ecap[0] = 0x3ff01;
    put(dmar + 80, 8, 0xfed92000);
    seal();
    assert(!build(2)); /* expanded IRO overlaps second unit */
    f.ecap[0] = 0x1001;
    put(dmar + 80, 8, 0xfeda0000);
    seal();
    dmar[76] = 0;
    seal();
    assert(!build(2)); /* uncovered media */
    reset();
    dmar[52] = 0;
    put(dmar + 50, 2, 24);
    scope(64, 2, 0, 4);
    length = 72;
    devices[0].header = 1;
    devices[0].secondary = devices[0].subordinate = 1;
    f.pcie[0] = 4;
    devices[1].bdf = 0x100;
    devices[1].parent = 0x20;
    devices[1].owner = PCI_OWNER_GUEST;
    seal();
    assert(build(2) && walk(0, 0x100, 0x2000, 1));
    devices[1].owner = PCI_OWNER_VMM;
    devices[2] = (struct pci_device){.bdf = 0x108, .parent = 0x20, .owner = PCI_OWNER_GUEST};
    assert(!build(3)); /* shared downstream ownership */
    devices[2].owner = PCI_OWNER_VMM;
    f.pcie[0] = 7;
    assert(!build(3)); /* PCIe-to-PCI alias */
    f.pcie[0] = 255;
    assert(!build(3)); /* conventional PCI alias */
    reset();
    put(dmar + 50, 2, 24);
    scope(64, 5, 0, 4);
    length = 72;
    seal();
    assert(!build(2));
    dmar[64] = 1;
    dmar[65] = 7;
    seal();
    assert(!build(2));
    reset();
    put(dmar + 50, 2, 24);
    scope(64, 1, 0, 4);
    length = 72;
    seal();
    assert(!build(2)); /* all+endpoint */
}
static void reserved(void)
{
    reset();
    length = 96;
    put(dmar + 64, 2, 1);
    put(dmar + 66, 2, 32);
    put(dmar + 72, 8, 0x700000);
    put(dmar + 80, 8, 0x700fff);
    scope(88, 1, 0, 4);
    seal();
    assert(build(2) && walk(0, 0x20, 0x700000, 1));
    assert(!walk(0, 0x18, 0x700000, 2) && !walk(0, 0x20, 0x701000, 1));
    put(dmar + 72, 8, POOL_BASE);
    put(dmar + 80, 8, POOL_BASE + 4095);
    seal();
    assert(!build(2));
    put(dmar + 72, 8, 0x700001);
    put(dmar + 80, 8, 0x700fff);
    seal();
    assert(!build(2));
    put(dmar + 72, 8, 0x700000);
    put(dmar + 80, 8, 0x702fff);
    seal();
    assert(!build(2));
    put(dmar + 80, 8, 0x700fff);
    devices[1].bars[0] = (struct pci_bar){.base = 0x700000, .size = 4096};
    seal();
    assert(!build(2));
    devices[1].bars[0].size = 0;
    put(dmar + 80, 8, 0x700fff);
    length = 104;
    put(dmar + 66, 2, 40);
    scope(96, 1, 0, 3);
    seal();
    assert(!build(2)); /* same firmware buffer crosses guest/VMM ownership */
}
static void sequencing(void)
{
    for (unsigned stage = 1; stage <= 4; ++stage) {
        reset();
        f.fail_stage = stage;
        assert(build(2));
        assert(!amd64_vtd_enable(&state));
        assert(!(f.status[0] & (1u << 31)));
    }
    for (unsigned bad = 1; bad <= 2; ++bad) {
        reset();
        f.bad_granularity = bad;
        assert(build(2));
        assert(!amd64_vtd_enable(&state));
    }
    reset();
    assert(build(2));
    f.context[0] = UINT64_C(1) << 63;
    assert(!amd64_vtd_enable(&state) && !f.writes); /* no SRTP over pending CCMD */
    reset();
    assert(build(2));
    f.iotlb[0] = UINT64_C(1) << 63;
    assert(!amd64_vtd_enable(&state) && !f.writes);
    reset();
    assert(build(2));
    f.status[0] = 1u << 27;
    assert(!amd64_vtd_enable(&state) && !f.writes);
    reset();
    assert(build(2));
    f.pmen[0] = 1;
    assert(!amd64_vtd_enable(&state) && !f.writes);
    reset();
    f.status[0] = 1u << 26;
    assert(!build(2));
    reset();
    f.status[0] = 1u << 25;
    assert(!build(2));
    reset();
    f.ecap[0] = 0x20001;
    assert(build(2));
    assert(amd64_vtd_enable(&state)); /* IRO > 4K */
}
int main(void)
{
    capabilities();
    acs_isolation();
    ari_isolation();
    mapping();
    scopes();
    reserved();
    sequencing();
    puts(
        "test-vtd: DMA domains, denied holes, scopes/RMRR, feature rejection and activation passed");
}
