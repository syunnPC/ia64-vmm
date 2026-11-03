/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Native execution of one FP expansion (engine/tier1/fpfast.h) for the
 * tests: the product's walker emits the rule with its inputs read from a
 * block at R14 and its result slots stored there, wrapped as a function
 * returning 1 at $DONE and 0 at $BAIL. Result slots not stored keep
 * FPX_POISON.
 */
#ifndef IA64VMM_TESTS_TIER1_FPX_NATIVE_H
#define IA64VMM_TESTS_TIER1_FPX_NATIVE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "engine/tier1/fpfast.h"

struct fpx_block {
    uint64_t in[T1_FPX_INPUTS], out[T1_FPX_SLOTS];
};
typedef uint64_t fpx_fn(struct fpx_block *);
#define FPX_POISON UINT64_C(0xa5a5a5a5a5a5a5a5)

/* CPUID.1:ECX FMA, OSXSAVE and AVX with XCR0 SSE and AVX state. */
static inline bool fpx_host_fma3(void)
{
    uint32_t a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if ((c & (1u << 12 | 1u << 27 | 1u << 28)) != (1u << 12 | 1u << 27 | 1u << 28))
        return false;
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;
}
static inline void fpx_op(struct t1_x86_buf *b, uint16_t op, uint8_t r, int64_t imm,
                          const struct t1_x86_mem *m)
{
    struct t1_x86_insn i = {.op = op};
    i.opnd[0].reg = r;
    i.opnd[1].imm = imm;
    if (m)
        i.mem = *m;
    t1_x86_emit(b, &i, NULL);
}
static inline size_t fpx_native_x86(void *ctx, const struct t1_x86_insn *i)
{
    size_t f;
    t1_x86_emit(ctx, i, &f);
    return f;
}
static inline void fpx_native_in(void *ctx, unsigned input)
{
    struct t1_x86_mem m = {T1_R14, T1_X86_NOREG, 0,
                           (int32_t)(offsetof(struct fpx_block, in) + 8 * input)};
    fpx_op(ctx, T1_X86_LOAD64, T1_R11, 0, &m);
}
static inline int32_t fpx_native_out(void *ctx, unsigned slot)
{
    (void)ctx;
    return (int32_t)(offsetof(struct fpx_block, out) + 8 * slot);
}

/* push r14; mov r14, rdi; the expansion; $DONE: eax = 1, $BAIL: eax = 0;
 * pop r14; ret. buf (bytes) assembles, page (mapped, bytes) executes. */
static inline bool fpx_native_run(const struct t1_fpx_rule *r, int64_t sf, int64_t mf,
                                  struct fpx_block *blk, unsigned *exit, uint8_t *buf, void *page,
                                  size_t bytes)
{
    struct t1_x86_buf b = {buf, 0, bytes, false};
    fpx_op(&b, T1_X86_PUSH64_R, T1_R14, 0, NULL);
    struct t1_x86_insn mv = {.op = T1_X86_MOV64_RR};
    mv.opnd[0].reg = T1_R14;
    mv.opnd[1].reg = T1_RDI;
    t1_x86_emit(&b, &mv, NULL);
    uint32_t bail[64], done[8];
    unsigned nbail = 0, ndone = 0;
    struct t1_fpx_io io = {&b, fpx_native_x86, fpx_native_in, fpx_native_out, &b, {sf, mf}};
    if (!t1_fpx_emit(r, &io, bail, &nbail, 64, done, &ndone, 8))
        return false;
    size_t at_done = b.size;
    fpx_op(&b, T1_X86_MOV32_RI32, T1_RAX, 1, NULL);
    fpx_op(&b, T1_X86_POP64_R, T1_R14, 0, NULL);
    fpx_op(&b, T1_X86_RET, 0, 0, NULL);
    size_t at_bail = b.size;
    fpx_op(&b, T1_X86_MOV32_RI32, T1_RAX, 0, NULL);
    fpx_op(&b, T1_X86_POP64_R, T1_R14, 0, NULL);
    fpx_op(&b, T1_X86_RET, 0, 0, NULL);
    for (unsigned k = 0; k < ndone; ++k)
        t1_x86_patch_rel32(&b, done[k], at_done);
    for (unsigned k = 0; k < nbail; ++k)
        t1_x86_patch_rel32(&b, bail[k], at_bail);
    if (b.failed || mprotect(page, bytes, PROT_READ | PROT_WRITE))
        return false;
    memcpy(page, buf, b.size);
    if (mprotect(page, bytes, PROT_READ | PROT_EXEC))
        return false;
    fpx_fn *fn;
    memcpy(&fn, &page, sizeof(fn));
    for (unsigned k = 0; k < T1_FPX_SLOTS; ++k)
        blk->out[k] = FPX_POISON;
    *exit = (unsigned)fn(blk);
    return true;
}
#endif
