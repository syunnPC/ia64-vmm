/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_VM_H
#define IA64VMM_VM_H
#include "ia64vmm/bootinfo.h"
enum vmm_exit_code {
    VMM_EXIT_SUCCESS = 0,
    VMM_EXIT_FAILURE = 1,
    VMM_EXIT_UNIMPLEMENTED = 2,
    VMM_EXIT_BAD_ACCESS = 3,
    VMM_EXIT_SETUP = 4,
    VMM_EXIT_TIMEOUT = 5,
};
/* Runs the platform and engine after core builds the guest map.
 * The copied boot info remains valid until hal_shutdown(). */
int vm_run_guest(const struct boot_info *bi);
#endif
