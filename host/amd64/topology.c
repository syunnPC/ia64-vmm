/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/topology.h"

static unsigned width(unsigned count)
{
    unsigned bits = 0;
    while (count > 1) {
        ++bits;
        count = (count + 1) / 2;
    }
    return bits;
}
static uint32_t mask(unsigned bits)
{
    return bits == 32 ? UINT32_MAX : (UINT32_C(1) << bits) - 1;
}

bool amd64_topology(struct boot_cpu_info *cpu, amd64_cpuid_fn query)
{
    uint32_t a, b, c, d, max, vendor;
    query(0, 0, &max, &vendor, &c, &d);
    cpu->topology_valid = false;
    cpu->core_type = 0;
    if (max >= 0x1a) {
        query(0x1a, 0, &a, &b, &c, &d);
        cpu->core_type = a >> 24;
    }
    /* SDM Vol.3 processor topology: the highest enumerated level defines
     * the package boundary, including module/tile/die levels on leaf 1F.
     * APIC IDs may have gaps; only CPUID-defined bit fields are used. */
    uint32_t leaves[] = {0x1f, 0xb};
    for (unsigned l = 0; l < 2; ++l) {
        if (max < leaves[l])
            continue;
        unsigned smt = 0, package = 0;
        bool valid = false;
        uint32_t apic = 0;
        for (unsigned sub = 0; sub < 32; ++sub) {
            query(leaves[l], sub, &a, &b, &c, &d);
            unsigned type = (c >> 8) & 255;
            if (!(b & 0xffff) || !type)
                break;
            unsigned shift = a & 31;
            if (shift < package)
                return false;
            if (type == 1)
                smt = shift;
            package = shift;
            apic = d;
            valid = true;
        }
        if (!valid)
            continue;
        if (apic != cpu->apic_id || smt > package)
            return false;
        cpu->smt_id = apic & mask(smt);
        cpu->core_id = (apic >> smt) & mask(package - smt);
        cpu->package_id = apic >> package;
        cpu->topology_leaf = leaves[l];
        cpu->topology_valid = true;
        return true;
    }
    query(0x80000000, 0, &a, &b, &c, &d);
    uint32_t extended = a;
    query(1, 0, &a, &b, &c, &d);
    unsigned logical = d & (1u << 28) ? (b >> 16) & 255 : 1;
    uint32_t initial_id = b >> 24;
    if (!logical)
        logical = 1;
    if (vendor == UINT32_C(0x68747541) && extended >= 0x80000008) {
        query(0x80000001, 0, &a, &b, &c, &d);
        bool topoext = c & (1u << 22);
        query(0x80000008, 0, &a, &b, &c, &d);
        unsigned package = (c >> 12) & 15;
        if (!package)
            package = width((c & 255) + 1);
        if (topoext && extended >= 0x8000001e) {
            query(1, 0, &a, &b, &c, &d);
            unsigned family = (a >> 8) & 15;
            if (family == 15)
                family += (a >> 20) & 255;
            /* Pre-Zen leaf 1E describes compute units, not SMT cores. Do
             * not silently treat a Bulldozer module as one physical core. */
            if (family < 0x17)
                return false;
            query(0x8000001e, 0, &a, &b, &c, &d);
            if (a != cpu->apic_id)
                return false;
            unsigned smt = width(((b >> 8) & 255) + 1);
            if (smt > package)
                return false;
            cpu->core_id = b & 255;
            cpu->smt_id = a & mask(smt);
            cpu->package_id = a >> package;
            cpu->topology_leaf = 0x8000001e;
        } else {
            /* Older AMD without TOPOEXT: NC is cores minus one. SMT is
             * absent; reject inconsistent logical/core counts. */
            if (logical != (c & 255) + 1 || initial_id != cpu->apic_id)
                return false;
            cpu->core_id = initial_id & mask(package);
            cpu->smt_id = 0;
            cpu->package_id = initial_id >> package;
            cpu->topology_leaf = 0x80000008;
        }
    } else if (vendor == UINT32_C(0x756e6547) && max >= 4) {
        query(4, 0, &a, &b, &c, &d);
        if (!(a & 31) || initial_id != cpu->apic_id)
            return false;
        unsigned cores = ((a >> 26) & 63) + 1;
        if (logical < cores || logical % cores)
            return false;
        unsigned smt = width(logical / cores), package = width(logical);
        cpu->core_id = (initial_id >> smt) & mask(package - smt);
        cpu->smt_id = initial_id & mask(smt);
        cpu->package_id = initial_id >> package;
        cpu->topology_leaf = 4;
    } else if (logical == 1 && initial_id == cpu->apic_id) {
        cpu->core_id = cpu->smt_id = 0;
        cpu->package_id = initial_id;
        cpu->topology_leaf = 1;
    } else
        return false;
    cpu->topology_valid = true;
    return true;
}

static bool same_core(const struct boot_cpu_info *a, const struct boot_cpu_info *b)
{
    return a->package_id == b->package_id && a->core_id == b->core_id;
}

bool amd64_placement(struct boot_info *bi, enum amd64_placement_mode mode, unsigned count)
{
    bool logical = mode == AMD64_PLACE_LOGICAL_THREAD;
    if (!count || bi->host_cpu_count > BOOT_MAX_CPUS || bi->management_cpu >= bi->host_cpu_count)
        return false;
    for (unsigned i = 0; i < bi->host_cpu_count; ++i) {
        bi->cpus[i].vcpu_index = UINT32_MAX;
        bi->cpus[i].management = i == bi->management_cpu;
        if (!logical && !bi->cpus[i].topology_valid)
            return false;
    }
    unsigned placed = 0;
    /* Keep MP ordering within each class. Unknown/non-hybrid cores follow
     * P cores, then E cores; never infer type from APIC numbering. The
     * management CPU is never a vCPU. physical-core also excludes its core;
     * core mode lets a sibling thread of it take that core's one vCPU. */
    for (unsigned pass = 0; pass < 3; ++pass)
        for (unsigned i = 0; i < bi->host_cpu_count && placed < count; ++i) {
            struct boot_cpu_info *cpu = &bi->cpus[i];
            unsigned rank = cpu->core_type == 0x40 ? 0 : cpu->core_type == 0x20 ? 2 : 1;
            if (rank != pass)
                continue;
            if (cpu->management || (mode == AMD64_PLACE_PHYSICAL_CORE &&
                                    same_core(cpu, &bi->cpus[bi->management_cpu])))
                continue;
            bool used = false;
            for (unsigned j = 0; !logical && j < bi->host_cpu_count; ++j)
                used |= bi->cpus[j].vcpu_index != UINT32_MAX && same_core(cpu, &bi->cpus[j]);
            if (!used)
                cpu->vcpu_index = placed++;
        }
    bi->vcpu_count = placed;
    return placed == count;
}

/* An unverified topology (logical-thread placement only) cannot rule out a
 * shared core. */
static bool may_share_core(const struct boot_cpu_info *a, const struct boot_cpu_info *b)
{
    return !a->topology_valid || !b->topology_valid || same_core(a, b);
}

unsigned amd64_helpers(const struct boot_info *bi, unsigned *cpu, unsigned *rank)
{
    /* Helper ranks: rank 1, a core without vCPU or management;
     * 2, the management CPU's SMT sibling; 3, a vCPU's SMT sibling. A core
     * shared with a vCPU is rank 3 even if it also holds the management CPU:
     * the helper competes with that vCPU. MP order within a rank, in every
     * placement mode. */
    unsigned count = 0;
    for (unsigned want = 1; want <= 3; ++want)
        for (unsigned i = 0; i < bi->host_cpu_count; ++i) {
            const struct boot_cpu_info *helper = &bi->cpus[i];
            if (helper->management || helper->vcpu_index != UINT32_MAX)
                continue;
            unsigned r = 1;
            for (unsigned j = 0; j < bi->host_cpu_count; ++j) {
                const struct boot_cpu_info *other = &bi->cpus[j];
                if (j == i || !may_share_core(helper, other))
                    continue;
                if (other->vcpu_index != UINT32_MAX)
                    r = 3;
                else if (other->management && r < 2)
                    r = 2;
            }
            if (r != want)
                continue;
            cpu[count] = i;
            rank[count++] = r;
        }
    return count;
}
