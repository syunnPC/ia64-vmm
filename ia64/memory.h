/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_IA64_MEMORY_H
#define IA64VMM_IA64_MEMORY_H
#include "ia64/semantics.h"
#include "ia64vmm/guest_mem.h"

/* Infrastructure for fetch and the following memory-instruction priority.
 * Callers validate alignment and translation before these physical accesses. */
struct ia64_result ia64_physical_read(struct vcpu *v, gpa_t gpa, unsigned size, uint64_t *value);
struct ia64_result ia64_physical_write(struct vcpu *v, gpa_t gpa, unsigned size, uint64_t value);
/* Ordered instruction accesses; the ordinary helpers above are relaxed. */
struct ia64_result ia64_physical_read_ordered(struct vcpu *v, gpa_t gpa, unsigned size,
                                              uint64_t *value, bool acquire);
struct ia64_result ia64_physical_write_ordered(struct vcpu *v, gpa_t gpa, unsigned size,
                                               uint64_t value, bool release);
/* Integer-only atomic bundle snapshot; false if the host lacks CX16. */
bool ia64_atomic_read16(void *ptr, uint64_t *low, uint64_t *high);
const struct gm_region *ia64_physical_region(struct vcpu *v, gpa_t gpa, unsigned size);
/* Device read-modify-writes (semaphores, 16-byte references) hold this
 * VM-wide lock around their transactions; device ops never take it. */
void ia64_device_rmw_begin(void);
void ia64_device_rmw_end(void);
/* Memory image of a datum under UM.be or RSC.be (SDM Vol.1 §3.2.3): the
 * byte order is reversed within the size bytes; byte accesses are equal. */
static inline uint64_t ia64_byte_order(uint64_t value, unsigned size, bool big_endian)
{
    if (!big_endian)
        return value;
    switch (size) {
    case 2:
        return __builtin_bswap16((uint16_t)value);
    case 4:
        return __builtin_bswap32((uint32_t)value);
    case 8:
        return __builtin_bswap64(value);
    default:
        return value;
    }
}
struct ia64_result ia64_memory16(struct vcpu *v, const Ia64Instruction *insn);
struct ia64_result ia64_memory_hint(struct vcpu *v, const Ia64Instruction *insn);
#endif
