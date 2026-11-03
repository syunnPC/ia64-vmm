/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "firmware/new/fpswa/fpswa.h"
#include "ia64/decoder.h"

int main(void)
{
    char line[12000];
    while (fgets(line, sizeof(line), stdin)) {
        struct fpswa_fr memory[128] __attribute__((aligned(16))) = {0};
        struct fpswa_state state = {UINT64_MAX, UINT64_MAX,  memory + 2,
                                    memory + 6, memory + 16, memory + 32};
        uint64_t ipsr = UINT64_C(1) << 41, fpsr = 0, isr = UINT64_C(1) << 41 | 8, pr = 1, ifs = 0,
                 raw = 0;
        uint64_t bundle[2] = {0};
        struct fpswa_call call = {1, bundle, &ipsr, &fpsr, &isr, &pr, &ifs, &state, {{0}}};
        unsigned slot = 1, bad = 0;
        for (char *t = strtok(line, " \n"); t; t = strtok(NULL, " \n")) {
            char *eq = strchr(t, '=');
            if (!eq)
                continue;
            *eq++ = 0;
            uint64_t value = strtoull(eq, NULL, 16);
            if (!strcmp(t, "raw"))
                raw = value;
            else if (!strcmp(t, "fpsr"))
                fpsr = value;
            else if (!strcmp(t, "psr"))
                ipsr = value | (UINT64_C(1) << 41);
            else if (!strcmp(t, "isr"))
                isr = value;
            else if (!strcmp(t, "pr"))
                pr = value;
            else if (!strcmp(t, "ifs"))
                ifs = value;
            else if (!strcmp(t, "trap"))
                call.trap_type = value;
            else if (!strcmp(t, "masklow"))
                state.mask_low = value;
            else if (!strcmp(t, "maskhigh"))
                state.mask_high = value;
            else if (!strcmp(t, "slot"))
                slot = value;
            else if (!strcmp(t, "bad"))
                bad = value;
            else if (t[0] == 'f') {
                unsigned r = strtoul(t + 1, NULL, 10), sign, exp;
                unsigned long long mant;
                if (r < 128 && sscanf(eq, "%u:%x:%llx", &sign, &exp, &mant) == 3)
                    memory[r] = (struct fpswa_fr){mant, exp | ((uint64_t)sign << 17)};
            }
        }
        memcpy(call.live, memory, sizeof(memory));
        bundle[0] = slot == 2 ? 0xf : 0xd;
        if (slot == 1) {
            bundle[0] |= raw << 46;
            bundle[1] = raw >> 18;
        } else if (slot == 2)
            bundle[1] = raw << 23;
        ipsr = (ipsr & ~(UINT64_C(3) << 41)) | ((uint64_t)slot << 41);
        isr = (isr & ~(UINT64_C(3) << 41)) | ((uint64_t)slot << 41);
        if (bad == 1)
            call.bundle = NULL;
        if (bad == 2)
            state.low_volatile = NULL;
        struct fpswa_ret ret = fpswa_emulate(&call);
        printf("status=%llx err=%llx fpsr=%llx psr=%llx isr=%llx pr=%llx ifs=%llx",
               (unsigned long long)ret.status, (unsigned long long)ret.err1,
               (unsigned long long)fpsr, (unsigned long long)(ipsr & ~(UINT64_C(3) << 41)),
               (unsigned long long)(isr & ~(UINT64_C(3) << 41)), (unsigned long long)pr,
               (unsigned long long)ifs);
        for (unsigned r = 2; r < 128; ++r) {
            struct fpswa_fr f =
                ((r < 64 ? state.mask_low : state.mask_high) & (UINT64_C(1) << (r % 64)))
                    ? memory[r]
                    : call.live[r];
            printf(" f%u=%u:%05x:%016llx", r, (unsigned)(f.sign_exponent >> 17) & 1,
                   (unsigned)f.sign_exponent & 0x1ffff, (unsigned long long)f.significand);
        }
        putchar('\n');
    }
    return 0;
}
