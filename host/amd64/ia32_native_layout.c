/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/ia32_native.h"

bool amd64_ia32_map_leaf(enum ia32_backend_kind backend, bool execute_only, unsigned bits,
                         const struct ia32_mapping *m, uint64_t *leaf)
{
    if (!m || !leaf || bits < 32 || bits > 52 ||
        (backend != IA32_BACKEND_SVM && backend != IA32_BACKEND_VMX) ||
        ((m->token.value | m->hpa) & 4095) || m->token.value >= (UINT64_C(1) << 48) ||
        m->hpa >= (UINT64_C(1) << bits) || (m->rights & ~7u) ||
        (unsigned)m->memory_type > IA32_MEMORY_WC)
        return false;
    unsigned rights = m->rights;
    if (!rights) {
        *leaf = 0;
        return true;
    }
    if (!(rights & IA32_MAP_READ) &&
        (backend == IA32_BACKEND_SVM || (rights & IA32_MAP_WRITE) || !execute_only))
        return false;
    if (backend == IA32_BACKEND_VMX) {
        const unsigned types[] = {6, 0, 1};
        *leaf = m->hpa | rights | (types[m->memory_type] << 3) | (1u << 6);
    } else {
        const unsigned pat[] = {0, 0x18, 0x98};
        *leaf = m->hpa | 5 | ((rights & IA32_MAP_WRITE) ? 2 : 0) | pat[m->memory_type] |
                ((rights & IA32_MAP_EXEC) ? 0 : UINT64_C(1) << 63);
    }
    return true;
}
