/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A one-vCPU executor sample, including core/smp.c's boundary operations.
 * A fixed fake engine isolates the executor cost from translation noise. */
#include <stdio.h>
#include <stdlib.h>
#include "core/smp.h"
#include "core/vm.h"
#include "core/log_internal.h"
#include "tests/hal/hal_linux.h"
#include "platform/platform.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/interrupts.h"
static struct platform_state platform;
static uint64_t slices, remaining;
struct vcpu_exit __wrap_engine_run(struct vcpu *v, uint64_t budget)
{
    (void)v;
    if (!budget)
        exit(1);
    ++slices;
    return (struct vcpu_exit){.reason = --remaining ? VCPU_EXIT_BUDGET : VCPU_EXIT_SHUTDOWN};
}
int main(int argc, char **argv)
{
    if (argc != 2 || !(remaining = strtoull(argv[1], NULL, 10)))
        return 1;
    log_set_level(LOG_ERROR);
    if (hal_linux_configure(false, 1, 0, NULL, 0))
        return 1;
    struct vm *vm = vm_create(1, &platform, false);
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg);
    if (!vm || !v)
        return 1;
    vm->cpu[0].v = v;
    vm->cpu[0].host_cpu = 0;
    if (!platform_irq_bind(v, 0, IA64VMM_CPU_LID(0), 0))
        return 1;
    platform.reset_requested = true;
    struct vm_exec exec = {.start = hal_counter(), .hz = hal_counter_hz()};
    uint64_t start = hal_counter();
    int rc = vm_run(vm, &exec, 0);
    uint64_t ns = hal_counter() - start;
    if (rc != VMM_EXIT_SUCCESS)
        return 1;
    printf("executor slices=%llu ns=%llu\n", (unsigned long long)slices, (unsigned long long)ns);
    vcpu_destroy(v);
    vm_destroy(vm);
    return 0;
}
