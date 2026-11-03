/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Benchmark job records (see docs/testing.md): the machine
 * state the hosted Tier1 harness gives a program (tests/tier1/programs.c,
 * captured by tests/bench/jobdump.c after vcpu_reset + guest_setup), the
 * memory it starts with, and, per iteration-count variant, the retired
 * instruction count and final register values of an interpreter run to
 * its end marker. The EFI wrapper (tests/bench/wrap/benchwrap.c) embeds a
 * file of these records and reproduces each state on the platform under
 * test. Every field is a little-endian uint64_t (both ends are LP64 LE).
 *
 * Relocation: the firmware of both platforms occupies guest physical
 * 0x100000-0x180000, so the wrapper places the job window at W and adds
 * D = W - BJ_RAM_BASE to every guest *physical* address the job holds:
 * the registers in reloc_gr/reloc_bank0, ip/iva/bspstore per
 * reloc_flags, the PA field of every TR (BJ_RELOC_TR), the memory words
 * in reloc_word (VHPT entries) and checks with reloc set. Virtual
 * addresses stay those of the harness; where an IVT handler maps VA
 * payload = PA (guest output pages, tests/tier1/programs/ivt.inc) the VA moves
 * with the PA (reloc_gr).
 */
#ifndef IA64VMM_TESTS_BENCH_JOB_H
#define IA64VMM_TESTS_BENCH_JOB_H

#define BJ_MAGIC 0x424f4a4234364149ull /* "IA64BJOB" */
#define BJ_VERSION 2
#define BJ_RAM_BASE 0x100000ull /* tests/tier1/harness.h T1H_RAM_* */
#define BJ_RAM_SIZE 0x100000ull
#define BJ_MAX_TR 4
#define BJ_MAX_PURGE 64
#define BJ_MAX_VARIANT 4
#define BJ_MAX_CHECK 16
#define BJ_MAX_MEM 32
#define BJ_MAX_STEP 256
#define BJ_MAX_RELOC 64

#define BJ_RELOC_IP 1ull
#define BJ_RELOC_IVA 2ull
#define BJ_RELOC_BSPSTORE 4ull
#define BJ_RELOC_TR 8ull

/* Where a checked value lives in the final state. */
enum { BJ_GR = 0, BJ_BANK0 = 1, BJ_STACKED = 2, BJ_PR = 3 };

struct bj_tr {
    unsigned long long side, slot, va, pte, itir;
}; /* side 0 = I, 1 = D */
struct bj_purge {
    unsigned long long va, ps, reloc;
}; /* ptc.l va (+ D), ps << 2 */
struct bj_check {
    unsigned long long where, index, value, reloc;
};
struct bj_variant {
    unsigned long long gr_index, gr_value; /* static GR overridden (0: none) */
    unsigned long long retired;            /* interpreter: instructions to the end break */
    unsigned long long ncheck;
    struct bj_check check[BJ_MAX_CHECK];
};
struct bj_mem {
    unsigned long long gpa, len, offset;
}; /* offset from the record */

struct bj_job {
    char name[32];
    unsigned long long size; /* bytes, including the memory data */
    unsigned long long ip, psr, rsc, bspstore, rnat, fpsr, lc, ec, pfs, unat, ccv;
    unsigned long long iva, pta, dcr, pr, nstacked;
    unsigned long long gr[32], bank0[16], br[8];
    unsigned long long rr_mask, rr[8];
    unsigned long long ntr;
    struct bj_tr tr[BJ_MAX_TR];
    unsigned long long npurge;
    struct bj_purge purge[BJ_MAX_PURGE];
    unsigned long long nvariant;
    struct bj_variant variant[BJ_MAX_VARIANT];
    unsigned long long nmem;
    struct bj_mem mem[BJ_MAX_MEM];
    unsigned long long reloc_gr, reloc_bank0, reloc_flags, nreloc_word;
    unsigned long long reloc_word[BJ_MAX_RELOC]; /* harness GPAs of 64-bit words */
};

/* One run of the plan: job index, variant index. */
struct bj_step {
    unsigned long long job, variant;
};

struct bj_file {
    unsigned long long magic, version, njob, nstep;
    unsigned long long job_offset[16]; /* from the file start */
    struct bj_step step[BJ_MAX_STEP];
};
#endif
