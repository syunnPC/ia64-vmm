/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Each guest memory instruction retains its own access and fault boundary.
 * A completed load does not grant write permission or stronger alignment. */
static void t_memory_reuse(void)
{
    const uint64_t prog = UM_CODE + 0x6000;
    for (unsigned test = 0; test < 9; ++test) {
        unsigned width = test == 1 ? 1 : 8;
        uint32_t flags = test == 5 ? T1_COMPILE_VERIFY :
                         test == 6 ? T1_COMPILE_STORE_LOG :
                         test == 7 ? T1_COMPILE_STORE_LOG | T1_COMPILE_UNDO_LOG : 0;
        struct ienc_bundle b[] = {
            {IENC_STOP(IENC_MMI), {ienc_ld(0, width, false, 14, 32), ienc_nop_m(), ienc_nop_i()}},
            {IENC_STOP(IENC_MII), {ienc_nop_m(), ienc_adds(0, 14, 1, 14), ienc_nop_i()}},
            {IENC_STOP(IENC_MMI), {ienc_st(0, 8, false, test == 3 ? 33 : 32, 14), ienc_nop_m(), ienc_nop_i()}},
            {IENC_STOP(IENC_MMI), {ienc_ld(0, 8, false, 15, test == 3 ? 33 : 32), ienc_nop_m(), ienc_nop_i()}},
            {IENC_STOP(IENC_MIB), {ienc_nop_m(), ienc_nop_i(), ienc_br_cloop(-4 * 16)}},
            {IENC_MIB, {ienc_nop_m(), ienc_nop_i(), ienc_br_cond(0, 0)}},
        };
        /* An acquire/release access keeps its fence as well as the access. */
        if (test == 4) {
            b[0].slot[0] = ienc_ld(0, 8, true, 14, 32);
            b[2].slot[0] = ienc_st(0, 8, true, 32, 14);
        }
        if (test == 8)
            b[1].slot[0] = ienc_ld(0, 8, false, 16, 33);
        um_emit(prog, b, sizeof(b) / sizeof(b[0]));
        struct vcpu *v = um_vcpu(prog), *ref = um_vcpu(prog);
        uint64_t addr = UM_DATA + 0x38000 + (test == 1);
        memset(um_ram(UM_DATA + 0x38000), 0x35, 32);
        code_notify_vmm_write(UM_DATA + 0x38000, 32);
        if (test == 2) {
            /* SDM Vol.2 PTE ar=0 permits reads, not writes. */
            v->s.psr |= IA64_PSR_DT;
            v->s.rr[0] = (UINT64_C(0x321) << 8) | (12 << 2);
            ia64_mmu_changed(v, IA64_MMU_RR, 0, 0);
            v->s.tr[1][0] = (struct ia64_tlb_entry){
                .va = addr, .pte = (addr & ~UINT64_C(4095)) | 1 | (1u << 5) | (1u << 6),
                .itir = 12 << 2, .rid = 0x321, .page_shift = 12, .valid = true
            };
            ia64_mmu_changed(v, IA64_MMU_TR, 1, 0);
        }
        um_gr(v, 32, addr, false);
        um_gr(v, 33, test == 8 ? UINT64_C(1) << hostwin_bits(umt_hostwin()) : addr + 8, false);
        v->s.ar[AR_LC] = 3;
        struct hostwin_vcpu *single = NULL;
        if (test == 8) {
            /* t_window_tags registered this RAM page. One slot forces
             * each intervening different-tag access to replace the bind. */
            struct hostwin_config cfg = {hostwin_bits(umt_hostwin()), 1, true};
            single = hostwin_create(&cfg, hostwin_linux_backend(), hostwin_linux_ctx());
            CHECK(single != NULL);
            struct t1_vcpu *tv = umt_tv(v);
            tv->hw = tv->x.hw = single;
        }
        struct um_snapshot snap;
        um_snapshot_take(&snap, v);
        uint32_t id = umt_compile(v, flags, 0, 0, NULL, "memory reuse");
        CHECK(id);
        const struct t1_region *r = t1_region_get(id);
        CHECK(r->nsites == 3 + (test == 8));
        unsigned tags = 0, deltas = 0, accesses = 0;
        for (uint32_t at = r->native_entry; at < r->code_size;) {
            struct t1_x86_decoded decoded;
            CHECK(t1_x86_decode(r->code + at, r->code_size - at, &decoded));
            for (unsigned site = 0; site < r->nsites; ++site) {
                tags += decoded.op == T1_X86_CMP64_RM && decoded.mem.base == T1_R14 &&
                        decoded.mem.disp == (int32_t)X_SITE_TAG(r->site_base + site);
                deltas += decoded.op == T1_X86_LOAD64 && decoded.mem.base == T1_R14 &&
                          decoded.mem.disp == (int32_t)X_SITE_DELTA(r->site_base + site);
            }
            at += decoded.length;
        }
        for (unsigned k = 0; k < r->nrip; ++k)
            accesses += r->rip[k].kind == T1_RIP_ACCESS;
        CHECK(accesses == r->nsites && deltas == r->nsites + (test == 7));
        CHECK(tags == (test == 8 ? 3 : flags ? 3 : test == 3 ? 2 : 1));
        struct umt_run run;
        umt_case(v, ref, &snap, flags, 80, id, NULL, &run, false, "memory reuse");
        CHECK(run.nexits != 0);
        if (single) {
            struct hostwin_stats stats;
            hostwin_get_stats(single, &stats);
            CHECK(stats.rebinds >= 8);
        }
        if (test == 1 || test == 2) {
            uint32_t follower = point_at(id, T1_PT_SITE, 1);
            bool seen = false;
            for (unsigned k = 0; k < run.nexits; ++k) {
                struct t1_exit_info exit;
                CHECK(t1_exit_get(umt_tv(v), k, &exit));
                if (exit.point_id == follower && exit.kind == T1_PT_SITE) {
                    const struct t1_region *r = t1_region_get(id);
                    CHECK(r->point[follower].ip == prog + 2 * 16);
                    CHECK(r->point[follower].slot == 0);
                    seen = true;
                }
            }
            CHECK(seen);
        }
        if (test == 5) {
            struct umt_arm arm = {.point = point_at(id, T1_PT_SITE, 1), .occurrence = 2,
                                  .action = T1_FORCE_TRAP_RETRY};
            umt_case(v, ref, &snap, flags, 80, id, &arm, &run, false, "memory reuse forced retry");
            CHECK(run.forced == 1 && run.retries > 0);
        }
        um_snapshot_free(&snap);
        umt_release(v);
        if (single)
            hostwin_destroy(single);
        vcpu_destroy(v);
        vcpu_destroy(ref);
    }
    printf("unit_runtime: memory reuse: every access and follower alignment/permission fault exact; checked logs and forced retry unchanged\n");
}
