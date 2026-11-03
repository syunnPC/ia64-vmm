/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Differential FP audit harness: executes one raw IA-64 instruction per input
 * line through the real decoder and ia64_execute(), then prints the visible
 * architectural state for comparison with tests/fuzz/fp/fpmodel.py.
 *
 * Input tokens (whitespace separated, any order):
 *   u=F|M  raw=<41-bit hex>  fpsr=<hex>  psr=<hex>  pr=<hex>
 *   f<N>=<sign>:<exp hex>:<significand hex>   r<N>=<hex>   n<N>=0|1
 *   m0=<hex> m1=<hex>  (16 bytes of RAM at MEMBASE, little-endian words)
 * Output: one line per input line with the fields listed in dump(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ia64/fp.h"
#include "ia64vmm/guest_mem.h"

#define CODE_BASE UINT64_C(0x100000)
#define MEMBASE UINT64_C(0x108000)
static uint8_t ram[0x10000] __attribute__((aligned(4096)));

static const char *kind_name(enum ia64_step_kind k)
{
    switch (k) {
    case IA64_STEP_COMPLETE:
        return "complete";
    case IA64_STEP_BRANCH:
        return "branch";
    case IA64_STEP_FAULT:
        return "fault";
    case IA64_STEP_EXIT:
        return "exit";
    }
    return "?";
}
static const char *fault_name(enum ia64_fault_kind f)
{
    switch (f) {
    case IA64_FAULT_NONE:
        return "none";
    case IA64_FAULT_ILLEGAL:
        return "illegal";
    case IA64_FAULT_NAT:
        return "nat";
    case IA64_FAULT_RESERVED:
        return "reserved";
    case IA64_FAULT_PRIVILEGED:
        return "privileged";
    case IA64_FAULT_BREAK:
        return "break";
    case IA64_FAULT_UNALIGNED:
        return "unaligned";
    case IA64_FAULT_UNSUPPORTED_DATA:
        return "unsupported_data";
    case IA64_FAULT_FP_DISABLED:
        return "fp_disabled";
    case IA64_FAULT_FP:
        return "fp";
    case IA64_FAULT_FP_TRAP:
        return "fp_trap";
    default:
        return "non-fp-fault";
    }
    return "?";
}

static void put_fr(const char *name, struct ia64_fr f)
{
    printf(" %s=%u:%05x:%016llx", name, f.sign ? 1u : 0u, f.exponent & 0x1ffff,
           (unsigned long long)f.significand);
}

int main(void)
{
    if (!gm_add_region(&(struct gm_region){.base = CODE_BASE,
                                           .size = sizeof(ram),
                                           .kind = GM_RAM,
                                           .name = "fpaudit",
                                           .hva = ram})) {
        fprintf(stderr, "fp_harness: cannot register RAM\n");
        return 2;
    }
    struct vcpu_config cfg = {.engine = ENGINE_INTERP, .itc_icount = true};
    struct vcpu *v = vcpu_create(&cfg);
    if (!v) {
        fprintf(stderr, "fp_harness: vcpu_create failed\n");
        return 2;
    }
    vcpu_reset(v, NULL);

    static char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == '#' || line[0] == '\n')
            continue; /* comments in saved cases */
        struct ia64_state *s = &v->s;
        memset(s->fr, 0, sizeof(s->fr));
        memset(s->gr, 0, sizeof(s->gr));
        memset(s->nat, 0, sizeof(s->nat));
        s->pr = 1;
        s->psr = 0;
        s->cfm = 0;
        s->ar[AR_FPSR] = 0;
        memset(ram + (MEMBASE - CODE_BASE), 0, 32);
        IA64SlotUnit unit = IA64_UNIT_F;
        uint64_t raws[64];
        unsigned nraw = 0;
        for (char *tok = strtok(line, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
            char *eq = strchr(tok, '=');
            if (!eq)
                continue;
            *eq = 0;
            const char *key = tok, *val = eq + 1;
            if (!strcmp(key, "u"))
                unit = val[0] == 'M' ? IA64_UNIT_M : IA64_UNIT_F;
            else if (!strcmp(key, "raw")) {
                /* A comma-separated list runs as a sequence (e.g. the SDM Vol.1
                 * 12.3.3 divide/sqrt sequences); it stops at the first
                 * instruction that does not complete. */
                for (char *p = (char *)val; *p && nraw < 64;) {
                    raws[nraw++] = strtoull(p, &p, 16);
                    if (*p == ',')
                        ++p;
                    else
                        break;
                }
            } else if (!strcmp(key, "fpsr"))
                s->ar[AR_FPSR] = strtoull(val, NULL, 16);
            else if (!strcmp(key, "psr"))
                s->psr = strtoull(val, NULL, 16);
            else if (!strcmp(key, "pr"))
                s->pr = strtoull(val, NULL, 16) | 1;
            else if (!strcmp(key, "m0") || !strcmp(key, "m1")) {
                uint64_t w = strtoull(val, NULL, 16);
                memcpy(ram + (MEMBASE - CODE_BASE) + (key[1] == '1' ? 8 : 0), &w, 8);
            } else if (key[0] == 'f') {
                unsigned n = (unsigned)strtoul(key + 1, NULL, 10);
                unsigned sign = 0, exp = 0;
                unsigned long long sig = 0;
                if (n < 128 && sscanf(val, "%u:%x:%llx", &sign, &exp, &sig) == 3)
                    s->fr[n] = (struct ia64_fr){sig, exp & 0x1ffff, sign != 0};
            } else if (key[0] == 'r') {
                unsigned n = (unsigned)strtoul(key + 1, NULL, 10);
                if (n < 128)
                    s->gr[n] = strtoull(val, NULL, 16);
            } else if (key[0] == 'n') {
                unsigned n = (unsigned)strtoul(key + 1, NULL, 10);
                if (n < 128)
                    s->nat[n] = val[0] == '1';
            }
        }
        if (!nraw) {
            printf("error=noraw\n");
            continue;
        }
        unsigned slot = unit == IA64_UNIT_M ? 0 : 1;
        Ia64Instruction insn;
        struct ia64_result r;
        for (unsigned k = 0;; ++k) {
            insn = ia64_decode_insn(unit, raws[k], CODE_BASE, (uint8_t)slot);
            r = ia64_execute(v, &insn);
            if (k + 1 == nraw || r.kind != IA64_STEP_COMPLETE)
                break;
        }
        uint64_t m0, m1;
        memcpy(&m0, ram + (MEMBASE - CODE_BASE), 8);
        memcpy(&m1, ram + (MEMBASE - CODE_BASE) + 8, 8);
        printf("kind=%s fault=%s info=%llx exit=%u op=%u fpsr=%llx psr=%llx pr=%llx",
               kind_name(r.kind), r.kind == IA64_STEP_FAULT ? fault_name(r.fault) : "none",
               (unsigned long long)(r.kind == IA64_STEP_FAULT || r.kind == IA64_STEP_BRANCH ? r.info
                                                                                            : 0),
               r.kind == IA64_STEP_EXIT ? (unsigned)r.exit : 0u, (unsigned)insn.opcode,
               (unsigned long long)s->ar[AR_FPSR], (unsigned long long)s->psr,
               (unsigned long long)s->pr);
        put_fr("f6", s->fr[6]);
        put_fr("f7", s->fr[7]);
        put_fr("f8", s->fr[8]);
        put_fr("f9", s->fr[9]);
        put_fr("f32", s->fr[32]);
        printf(" r8=%llx n8=%u r20=%llx n20=%u m0=%llx m1=%llx\n", (unsigned long long)s->gr[8],
               s->nat[8] ? 1u : 0u, (unsigned long long)s->gr[20], s->nat[20] ? 1u : 0u,
               (unsigned long long)m0, (unsigned long long)m1);
    }
    vcpu_destroy(v);
    return 0;
}
