/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Layout shared by initpmi.c and entry.S. */
#ifndef INITPMI_H
#define INITPMI_H

/* What an OS handler (OS_INIT, OS_MCA) saw at entry, SAL §4.8, §5.3. */
#define REC_R1 0
#define REC_R8 8
#define REC_R9 16
#define REC_R10 24
#define REC_R11 32
#define REC_R12 40
#define REC_R16 48
#define REC_R17 56
#define REC_R18 64
#define REC_R19 72
#define REC_R20 80
#define REC_PSR 88 /* mov r=psr: PSR{36:35} and PSR{31:0} */
#define REC_RSC 96
#define REC_LID 104
#define REC_COUNT 112 /* entries, stored last */
#define REC_AREA 128  /* the first 0x1d0 bytes of the min-state area */
#define REC_AREA_WORDS 58
#define REC_SIZE 640
/* Records in initpmi_records: OS_INIT monarch, OS_INIT non-monarch, OS_MCA. */
#define REC_INIT 0
#define REC_SLAVE REC_SIZE
#define REC_MCA (2 * REC_SIZE)
#define REC_RECORDS (3 * REC_SIZE)

/* What the SAL PMI handler saw, SDM Vol.2 §11.5.2. */
#define PMI_R24 0
#define PMI_R25 8
#define PMI_R26 16
#define PMI_R27 24
#define PMI_R28 32
#define PMI_R29 40
#define PMI_B0 48
#define PMI_PSR 56
#define PMI_RSC 64
#define PMI_COUNT 72
#define PMI_SIZE 80

/* initpmi_wait: the values it holds while waiting (p10-p15 as WAIT_PR),
 * and its result. */
#define SIG(n) (0x5149000000000000 + (n))
#define WAIT_PR 0x9400
#define WAIT_PR_MASK 0xfc00
#define WAIT_TIMEOUT 0x8000000000000000

#ifndef __ASSEMBLER__
struct initpmi_record {
    UINT64 r1, r8, r9, r10, r11, r12, r16, r17, r18, r19, r20, psr, rsc, lid, count, pad;
    UINT64 area[REC_AREA_WORDS];
    UINT64 tail[6];
};
struct initpmi_pmi {
    UINT64 r24, r25, r26, r27, r28, r29, b0, psr, rsc, count;
};
_Static_assert(sizeof(struct initpmi_record) == REC_SIZE &&
                   __builtin_offsetof(struct initpmi_record, psr) == REC_PSR &&
                   __builtin_offsetof(struct initpmi_record, count) == REC_COUNT &&
                   __builtin_offsetof(struct initpmi_record, area) == REC_AREA,
               "entry.S records");
_Static_assert(sizeof(struct initpmi_pmi) == PMI_SIZE &&
                   __builtin_offsetof(struct initpmi_pmi, count) == PMI_COUNT,
               "entry.S PMI record");
#endif
#endif
