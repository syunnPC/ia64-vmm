/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_TOPOLOGY_H
#define VMM_AMD64_TOPOLOGY_H
#include "ia64vmm/bootinfo.h"
typedef void (*amd64_cpuid_fn)(uint32_t, uint32_t, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
bool amd64_topology(struct boot_cpu_info *cpu, amd64_cpuid_fn query);
enum amd64_placement_mode {
    AMD64_PLACE_PHYSICAL_CORE,  /* one vCPU per core, management core excluded */
    AMD64_PLACE_LOGICAL_THREAD, /* any logical CPU except the management one */
    AMD64_PLACE_CORE            /* one vCPU per core; the management core uses a sibling thread */
};
bool amd64_placement(struct boot_info *bi, enum amd64_placement_mode mode, unsigned count);
/* After amd64_placement: helper CPU indices and ranks in rank order (the
 * contract of hal_helper_cpu/hal_helper_rank); returns their count. Each
 * array holds host_cpu_count entries. */
unsigned amd64_helpers(const struct boot_info *bi, unsigned *cpu, unsigned *rank);
#endif
