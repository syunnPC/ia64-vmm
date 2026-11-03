/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host SIMD baseline and XSAVE areas. Pure parts, tested hosted. */
#ifndef IA64VMM_AMD64_SIMD_H
#define IA64VMM_AMD64_SIMD_H

#include "ia64vmm/host_isa.h"

/* Baseline features the VMM is compiled for (BM_SIMD_CFLAGS), as bits of a
 * missing-feature mask. Intel SDM Vol.2 CPUID leaf 1 and leaf 0DH. */
#define AMD64_SIMD_FPU (1u << 0)    /* CPUID.1:EDX[0] */
#define AMD64_SIMD_FXSR (1u << 1)   /* CPUID.1:EDX[24] */
#define AMD64_SIMD_SSE (1u << 2)    /* CPUID.1:EDX[25] */
#define AMD64_SIMD_SSE2 (1u << 3)   /* CPUID.1:EDX[26] */
#define AMD64_SIMD_SSE3 (1u << 4)   /* CPUID.1:ECX[0] */
#define AMD64_SIMD_SSSE3 (1u << 5)  /* CPUID.1:ECX[9] */
#define AMD64_SIMD_SSE41 (1u << 6)  /* CPUID.1:ECX[19] */
#define AMD64_SIMD_SSE42 (1u << 7)  /* CPUID.1:ECX[20] */
#define AMD64_SIMD_POPCNT (1u << 8) /* CPUID.1:ECX[23] */
#define AMD64_SIMD_XSAVE (1u << 9)  /* CPUID.1:ECX[26] */
#define AMD64_SIMD_AVX (1u << 10)   /* CPUID.1:ECX[28] */
#define AMD64_SIMD_XCR0 (1u << 11)  /* CPUID.(0DH,0):EAX[2:0]: x87, SSE and AVX state */
#define AMD64_SIMD_ALL 0xfffu

/* State components the baseline needs in XCR0: x87, SSE and AVX. */
#define AMD64_XCR0_BASELINE UINT64_C(7)
/* AMX TILECFG and TILEDATA (XCR0 bits 17-18): never enabled. */
#define AMD64_XCR0_AMX (UINT64_C(3) << 17)
/* MXCSR and x87 control word at every C function boundary. */
#define AMD64_MXCSR_DEFAULT 0x1f80u
#define AMD64_FCW_DEFAULT 0x037fu
/* Legacy region and XSAVE header (SDM Vol.1 §13.4). */
#define AMD64_XSAVE_MIN_BYTES 576u

/* The baseline features this CPU lacks; 0 when all are present. */
unsigned amd64_simd_missing(host_isa_cpuid_fn cpuid);
/* Names of the features in missing, space separated; returns out. */
const char *amd64_simd_describe(unsigned missing, char *out, size_t size);
/* A standard-format XSAVE area that XRSTOR loads as the initial state with
 * MXCSR 1F80H. False if bytes cannot hold the legacy region and header. */
bool amd64_xstate_init(void *area, size_t bytes);
/* XCR0 for this host: every user state component the boot CPU supports
 * (CPUID.(0DH,0):EDX:EAX) except AMX. */
uint64_t amd64_xcr0_mask(uint64_t supported);

/* Bare metal (host/amd64/cpu.c): enable SSE, AVX and XSAVE state on this
 * CPU (CR0, CR4, XCR0, MXCSR, x87). The first call (the boot CPU) chooses
 * amd64_xcr0 with amd64_xcr0_mask; every later call sets that mask and
 * stops the VMM if this CPU does not support all of it. Returns the missing
 * baseline features; nothing is changed unless that is 0. General registers
 * only before it. */
unsigned amd64_simd_enable(void);
/* XCR0 on every CPU, and the XSAVE area size for it (CPUID.(0DH,0):EBX
 * after XSETBV on the boot CPU; no CPU's may exceed it). */
extern uint64_t amd64_xcr0;
extern unsigned amd64_xstate_bytes;
/* 4 KiB pages for one XSAVE area of amd64_xstate_bytes. */
static inline size_t amd64_xstate_pages(void)
{
    return (amd64_xstate_bytes + 4095u) / 4096u;
}
/* XSAVE the enabled state to save, then XRSTOR it from load (assembly,
 * host/amd64/xstate.S). Clobbers RAX, RCX and RDX only. */
void amd64_xstate_switch(void *save, const void *load);
/* The two XSAVE areas a native x86 guest's entry and exit swap; the layout
 * is shared with svm_vmrun.S and vmx_vmrun.S. */
struct amd64_xstate_pair {
    void *host, *guest;
};

#endif
