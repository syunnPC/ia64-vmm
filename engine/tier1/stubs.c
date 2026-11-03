/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared runtime stubs in the arena's first page, outside region MIR.
 * Entry ABI and shared epilogue: exec.h.
 *
 *   exit_save    save the 11 allocatable registers to x->regs, then:
 *   exit_nosave  x->raw_r13 = R13; try native handoff, else return
 *                T1_RET_POINT through the shared epilogue
 *   miss         reached by `call` from a site's miss path with x->point
 *                set: save, t1_window_miss(x), then either restore and
 *                return (retry the tag check) or drop the return address
 *                and exit
 *   helper       the same around t1_helper_call(x); the C side has
 *                written the helper's results into x->regs[], so the
 *                restore loads them
 *   trap_entry   the trap client's resume target:
 *                t1_trap_resolve(x); 0 = restore every GPR except RSP and
 *                R14 from x->regs, RFLAGS from x->rflags, jump to x->rip;
 *                else exit (x->regs holds the trap capture)
 * Stack: the body keeps RSP = 0 mod 16; a stub entered by `call` adjusts
 * by 8 before calling C.
 */
#include "engine/tier1/region.h"
#include "ia64vmm/string.h"

struct t1_stub_addrs t1_stub;

static struct t1_x86_insn I(uint16_t op) {
    struct t1_x86_insn i;
    memset(&i, 0, sizeof(i));
    i.op = op;
    i.opnd[0].reg = i.opnd[1].reg = T1_X86_NOREG;
    i.mem.base = i.mem.index = T1_X86_NOREG;
    return i;
}

static size_t
emit(struct t1_x86_buf *b, struct t1_x86_insn i)
{
    size_t fix = SIZE_MAX;
    t1_x86_emit(b, &i, &fix);
    return fix;
}

static void store_reg(struct t1_x86_buf *b, uint8_t r, int32_t disp)
{
    struct t1_x86_insn i = I(T1_X86_STORE64);
    i.mem = (struct t1_x86_mem){T1_R14, T1_X86_NOREG, 0, disp};
    i.opnd[1].reg = r;
    emit(b, i);
}
static void load_reg(struct t1_x86_buf *b, uint8_t r, int32_t disp)
{
    struct t1_x86_insn i = I(T1_X86_LOAD64);
    i.mem = (struct t1_x86_mem){T1_R14, T1_X86_NOREG, 0, disp};
    i.opnd[0].reg = r;
    emit(b, i);
}
static void reg_imm(struct t1_x86_buf *b, uint16_t op, uint8_t r, int64_t imm)
{
    struct t1_x86_insn i = I(op);
    i.opnd[0].reg = r;
    i.opnd[1].imm = imm;
    emit(b, i);
}
static void reg_reg(struct t1_x86_buf *b, uint16_t op, uint8_t r, uint8_t s)
{
    struct t1_x86_insn i = I(op);
    i.opnd[0].reg = r;
    i.opnd[1].reg = s;
    emit(b, i);
}
static void one_reg(struct t1_x86_buf *b, uint16_t op, uint8_t r)
{
    struct t1_x86_insn i = I(op);
    i.opnd[0].reg = r;
    emit(b, i);
}

static void save_all(struct t1_x86_buf *b)
{
    for (unsigned r = 0; r < T1_X86_NREGS; ++r)
        if (T1_WRAPPER_PRESERVED & T1_X86_REGBIT(r))
            store_reg(b, (uint8_t)r, (int32_t)(X_REGS + 8u * r));
}
static void restore_all(struct t1_x86_buf *b)
{
    for (unsigned r = 0; r < T1_X86_NREGS; ++r)
        if (T1_WRAPPER_PRESERVED & T1_X86_REGBIT(r))
            load_reg(b, (uint8_t)r, (int32_t)(X_REGS + 8u * r));
}

/* call fn(x) with RSP = 0 mod 16 on entry to this sequence. */
static void call_c(struct t1_x86_buf *b, uintptr_t fn)
{
    reg_reg(b, T1_X86_MOV64_RR, T1_RDI, T1_R14);
    reg_imm(b, T1_X86_MOV64_RI64, T1_RAX, (int64_t)fn);
    one_reg(b, T1_X86_CALL_R, T1_RAX);
}

/* A stub entered by `call`: save, call fn, restore and return on 0, else
 * exit with the registers already saved. */
static uintptr_t called_stub(struct t1_x86_buf *b, uint8_t *page, uintptr_t fn)
{
    uintptr_t at = (uintptr_t)page + b->size;
    save_all(b);
    reg_imm(b, T1_X86_SUB64_RI, T1_RSP, 8);
    call_c(b, fn);
    reg_imm(b, T1_X86_ADD64_RI, T1_RSP, 8);
    reg_reg(b, T1_X86_TEST32_RR, T1_RAX, T1_RAX);
    size_t exit_fix = emit(b, I(T1_X86_JCC + T1_CC_NE));
    restore_all(b);
    emit(b, I(T1_X86_RET));
    t1_x86_patch_rel32(b, exit_fix, b->size);
    reg_imm(b, T1_X86_ADD64_RI, T1_RSP, 8); /* the return address */
    size_t j = emit(b, I(T1_X86_JMP));
    t1_x86_patch_rel32(b, j, t1_stub.exit_nosave - (uintptr_t)page);
    return at;
}

bool t1_stubs_build(uint8_t *page, size_t bytes)
{
    struct t1_x86_buf b = {page, 0, bytes, false};
    /* exit_save / exit_nosave and the shared epilogue. */
    t1_stub.exit_save = (uintptr_t)page + b.size;
    save_all(&b);
    t1_stub.exit_nosave = (uintptr_t)page + b.size;
    store_reg(&b, T1_R13, (int32_t)X_RAW_R13);
    load_reg(&b, T1_RAX, (int32_t)offsetof(struct t1_exec, fast));
    reg_reg(&b, T1_X86_TEST64_RR, T1_RAX, T1_RAX);
    size_t fast = emit(&b, I(T1_X86_JCC + T1_CC_NE));
    size_t handoff = b.size;
    load_reg(&b, T1_RAX, (int32_t)X_HANDOFF);
    reg_reg(&b, T1_X86_TEST64_RR, T1_RAX, T1_RAX);
    size_t no_handoff = emit(&b, I(T1_X86_JCC + T1_CC_E));
    reg_reg(&b, T1_X86_MOV64_RR, T1_RDI, T1_R14);
    one_reg(&b, T1_X86_CALL_R, T1_RAX);
    reg_reg(&b, T1_X86_TEST64_RR, T1_RAX, T1_RAX);
    size_t finished = emit(&b, I(T1_X86_JCC + T1_CC_E));
    store_reg(&b, T1_RAX, (int32_t)X_RIP);
    load_reg(&b, T1_R13, (int32_t)X_BUDGET_ENTRY);
    struct t1_x86_insn next = I(T1_X86_JMP_M);
    next.mem = (struct t1_x86_mem){T1_R14, T1_X86_NOREG, 0, (int32_t)X_RIP};
    emit(&b, next);
    t1_x86_patch_rel32(&b, no_handoff, b.size);
    t1_x86_patch_rel32(&b, finished, b.size);
    reg_imm(&b, T1_X86_MOV32_RI32, T1_RAX, T1_RET_POINT);
    reg_imm(&b, T1_X86_ADD64_RI, T1_RSP, 8);
    static const uint8_t pops[] = {T1_R15, T1_R14, T1_R13, T1_R12, T1_RBP, T1_RBX};
    for (unsigned k = 0; k < sizeof(pops); ++k)
        one_reg(&b, T1_X86_POP64_R, pops[k]);
    emit(&b, I(T1_X86_RET));
    t1_stub.miss = called_stub(&b, page, (uintptr_t)&t1_window_miss);
    t1_stub.helper = called_stub(&b, page, (uintptr_t)&t1_helper_call);
    /* trap_entry: RSP is the region body's (0 mod 16). */
    t1_stub.trap_entry = (uintptr_t)page + b.size;
    call_c(&b, (uintptr_t)&t1_trap_resolve);
    reg_reg(&b, T1_X86_TEST32_RR, T1_RAX, T1_RAX);
    size_t ex = emit(&b, I(T1_X86_JCC + T1_CC_NE));
    t1_x86_patch_rel32(&b, ex, t1_stub.exit_nosave - (uintptr_t)page);
    for (unsigned r = 0; r < T1_X86_NREGS; ++r)
        if (r != T1_RSP && r != T1_R14)
            load_reg(&b, (uint8_t)r, (int32_t)(X_REGS + 8u * r));
    struct t1_x86_insn push = I(T1_X86_PUSH64_M);
    push.mem = (struct t1_x86_mem){T1_R14, T1_X86_NOREG, 0, (int32_t)X_RFLAGS};
    emit(&b, push);
    emit(&b, I(T1_X86_POPFQ));
    struct t1_x86_insn jm = I(T1_X86_JMP_M);
    jm.mem = (struct t1_x86_mem){T1_R14, T1_X86_NOREG, 0, (int32_t)X_RIP};
    emit(&b, jm);
    t1_x86_patch_rel32(&b, fast, b.size);
    t1_fast_emit(&b, handoff);
    return !b.failed;
}
