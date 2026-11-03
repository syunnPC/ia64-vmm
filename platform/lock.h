/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Platform/Synchronization/SpinLock.cpp
 */
#ifndef IA64VMM_PLATFORM_LOCK_H
#define IA64VMM_PLATFORM_LOCK_H
/* Acquire/release loop. Device locks are executor
 * only: leave IF enabled for host shootdowns, never wait for another vCPU.
 * Order: PM/NVRAM -> service; port -> CF8 -> PCI -> MSI-X;
 * VGA -> PCI -> MSI-X; IOSAPIC -> host IOAPIC. Service-port forwarding bypasses the
 * service lock, taking only the destination device locks.
 * Lifecycle HAL dispatch must happen outside every device lock. */
static inline void platform_lock(unsigned *lock)
{
    unsigned expected = 0;
    while (
        !__atomic_compare_exchange_n(lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
        expected = 0;
    }
}
static inline void platform_unlock(unsigned *lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}
#endif
