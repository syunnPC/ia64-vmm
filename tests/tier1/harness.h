/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tier1 test machine: the guest
 * physical layout, the RAM allocator, the test MMIO device and its
 * transaction log, the end-marker break hook, image loading, and the
 * per-vCPU run logs that the store log (storelog.c) and the device share.
 *
 * The harness owns these guest-physical ranges in build/tier1-test (other
 * suites in the same binary, e.g. hostwin tests, must not
 * register regions overlapping them):
 *   [0x100000, 0x200000)  RAM (T1H_RAM_*)
 *   [0x400000, 0x401000)  test MMIO device (GM_EMULATED)
 *   0x500000              guaranteed unmapped (VCPU_EXIT_BAD_ACCESS)
 */
#ifndef IA64VMM_TESTS_TIER1_HARNESS_H
#define IA64VMM_TESTS_TIER1_HARNESS_H
#include "ia64/state.h"
#include "tests/tier1/compare.h"

#define T1H_RAM_BASE UINT64_C(0x100000)
#define T1H_RAM_SIZE UINT64_C(0x100000)
#define T1H_RAM_PAGES ((unsigned)(T1H_RAM_SIZE >> 12))
#define T1H_DEVICE UINT64_C(0x400000)
#define T1H_UNMAPPED UINT64_C(0x500000)

/* Places inside RAM. Images are linked at 0x100000 (mk/tier1.mk); the guest
 * IVT is at image offset 0x8000 (tests/tier1/programs/ivt.inc). */
#define T1H_CODE UINT64_C(0x100000)
#define T1H_IVT UINT64_C(0x108000)
#define T1H_INDEX UINT64_C(0x110000)  /* index array, up to 2 pages */
#define T1H_STATUS UINT64_C(0x118000) /* gather-load-after-store status word page */
#define T1H_OUT UINT64_C(0x120000)    /* output array, 8 pages */
#define T1H_OUT_PAGES 8u
#define T1H_TABLE UINT64_C(0x1c0000) /* table, up to 48 pages */
#define T1H_TABLE_PAGES 48u
#define T1H_VHPT UINT64_C(0x1f0000) /* 32 KiB short-format VHPT */
#define T1H_BSP UINT64_C(0x1f8000)  /* RSE backing store */
/* gather-virt maps the table at this VA payload in region 2 (programs.c), so
 * that its pages straddle the boundary between VHPT pages 0 and 1. */
#define T1H_TABLE_VA UINT64_C(0x1f8000)

/* End markers (break.i immediates handled by the harness break hook). */
#define T1H_BREAK_END UINT64_C(0x1ffffe)        /* normal end of a program */
#define T1H_BREAK_UNEXPECTED UINT64_C(0x1ffffd) /* IVT vector a guest run must not reach */

#define T1H_DEVICE_MAX 64u
#define T1H_STORE_MAX 4096u
enum t1h_pre_kind { T1H_PRE_NONE, T1H_PRE_MEMORY, T1H_PRE_UNDO };

/*
 * Everything one run produces besides the vCPU and RAM. One instance per
 * vCPU (t1h_bind): the device ops and v->memory_hook only receive the vCPU.
 * The pairing fields belong to storelog.c/reference.c.
 */
struct t1h_logs {
    struct compare_device_record device[T1H_DEVICE_MAX];
    size_t device_n;
    struct compare_store_record stores[T1H_STORE_MAX];
    size_t store_n;
    uint64_t stores_outside; /* completed integer stores outside the declared ranges */
    /* The bytes each store replaced, where known: read by
     * the logging proxy before its write (T1H_PRE_MEMORY), or from a native
     * record's undo log (T1H_PRE_UNDO, T1_COMPILE_UNDO_LOG regions). */
    uint64_t store_pre[T1H_STORE_MAX];
    uint8_t store_pre_kind[T1H_STORE_MAX];
    bool stores_on; /* store log attached (storelog_attach) */
    /* GVA pairing (storelog.c): the boundary stepped by reference.c and the
     * GVA predicted there from the decoded store and pre-state. */
    bool boundary_valid, predicted_valid;
    uint64_t boundary_ip, predicted_gva;
    unsigned boundary_slot, predicted_width;
    uint64_t paired_stepped, paired_callback; /* statistics */
    /* Break hook results. */
    unsigned breaks;
    uint64_t last_break_imm, last_break_ip;
    /* First harness error of the run (log overflow, unpairable store, ...);
     * a run with errors never compares equal (t1h_logs_ok). */
    unsigned errors;
    char error[200];
};

/* RAM allocator for the test RAM (default: anonymous mmap, 4 KiB aligned).
 * installs the hosted window backend's memfd RAM
 * (hostwin_linux_ram) before t1h_init so that windows,
 * interpreter and helpers see the same pages. */
typedef uint8_t *(*t1h_ram_alloc_fn)(gpa_t base, uint64_t size);
void t1h_set_ram_allocator(t1h_ram_alloc_fn fn);
/* Registers RAM and the device, installs the break hook. Idempotent. */
void t1h_init(void);
uint8_t *t1h_ram(void);

/* Image lookup by basename among the command-line paths (main.c). */
void t1h_set_images(int argc, char **argv);
const char *t1h_image(const char *basename);
/* Zeroes the RAM and copies the image to T1H_CODE (VMM writes: the code
 * caches are notified). */
void t1h_load_image(const char *basename);

/* Run logs. */
void t1h_bind(struct vcpu *v, struct t1h_logs *logs);
void t1h_unbind(struct vcpu *v);
struct t1h_logs *t1h_logs_of(const struct vcpu *v);
void t1h_logs_reset(struct t1h_logs *logs); /* empties logs, keeps stores_on */
void t1h_error(struct t1h_logs *logs, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool t1h_logs_ok(const struct t1h_logs *logs);

/* Bank-0 static register of a vCPU, wherever PSR.bn currently keeps it
 * (the IVT handlers' counters, tests/tier1/programs/ivt.inc). */
uint64_t t1h_bank0(const struct vcpu *v, unsigned reg);
/* Bank-1 r16-r31 (the programs run with PSR.bn = 1; a run can end inside a
 * handler, e.g. after a completion trap under PSR.ss). */
uint64_t t1h_bank1(const struct vcpu *v, unsigned reg);

/* Hash of one 4 KiB page (in-place captures). */
uint64_t t1h_page_hash(const uint8_t *page);

/* Failure reporting shared by the suites. */
#define T1H_CHECK(c)                                                                               \
    do {                                                                                           \
        if (!(c))                                                                                  \
            t1h_fail(__FILE__, __LINE__, #c);                                                      \
    } while (0)
_Noreturn void t1h_fail(const char *file, unsigned line, const char *what);
/* Label of comparisons that must find a difference (negative tests): their
 * differences are not printed. */
#define T1H_EXPECTED_DIFFERENCE "(expected difference)"
bool t1h_quiet(const char *label);
/* compare_diff_fn that prints the first `limit` differences to stderr. */
struct t1h_diff_printer {
    const char *label;
    unsigned printed, limit;
};
void t1h_print_diff(const char *what, uint64_t index, uint64_t reference, uint64_t actual,
                    void *arg);
#endif
