/* SPDX-License-Identifier: GPL-2.0-or-later */
static void frame_compare(struct vcpu *v, struct vcpu *ref, unsigned count)
{
    *ref = *v; ref->cfg.engine = ENGINE_INTERP;
    ref->check_workspace = NULL; ref->check_workspace_pages = 0;
    struct vcpu_exit expected = interp_run(ref, count), actual = engine_run(v, count);
    CHECK(actual.reason == expected.reason && actual.ip == expected.ip && actual.slot == expected.slot);
    CHECK(!memcmp(&v->s, &ref->s, sizeof(v->s)) && v->retired == ref->retired);
    CHECK(v->fault.kind == ref->fault.kind && v->fault.ip == ref->fault.ip);
}
static void test_frame_keys(struct vcpu *v, struct vcpu *ref)
{
    for (enum engine_kind engine = ENGINE_TIER0; engine <= ENGINE_LOCKSTEP; ++engine) {
        code_cache_flush();
        bundle(BASE, 1, nop, add(32,32,33), add(127,127,32));
        bundle(BASE + 16, 0x11, nop, nop, validator_branch(BASE + 16, BASE));
        restart(v, engine, BASE);
        struct tier0_stats before, after; tier0_get_stats(&before);
        for (unsigned sor = 8; sor <= 96; sor += 8)
            for (unsigned bof = 0; bof < 96; ++bof)
                for (unsigned rotation = 0; rotation < sor; ++rotation) {
                    v->s.ip = BASE; v->s.slot = 0; v->s.rse_bof = bof;
                    v->s.cfm = 96 | (UINT64_C(96) << 7) | ((uint64_t)(sor / 8) << 14) |
                        ((uint64_t)rotation << 18);
                    for (unsigned p = 32; p < 128; ++p) {
                        v->s.gr[p] = random64(); v->s.nat[p] = (p + rotation) % 3 == 0;
                    }
                    frame_compare(v, ref, 6);
                }
        tier0_get_stats(&after);
        CHECK(after.translations == before.translations + 12);
        /* Cached homes (tier0.c homes_for): a warm region re-entered with
         * only BOF, only a rotating base, or the same frame; then a new
         * region in the same arena slot with the same frame (flush). */
        const unsigned frames[][2] = {{80,3},{80,3},{10,3},{10,3},{80,3},{80,5},{80,3},{80,3}};
        for (unsigned round = 0; round < 2; ++round) {
            if (round) {
                code_cache_flush();
                bundle(BASE, 1, nop, add(40,41,47), add(46,46,40));
            }
            for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(frames); ++n) {
                v->s.ip = BASE; v->s.slot = 0; v->s.rse_bof = frames[n][0];
                v->s.cfm = 48 | (UINT64_C(48) << 7) | (UINT64_C(2) << 14) | ((uint64_t)frames[n][1] << 18);
                frame_compare(v, ref, 6);
            }
        }
        /* FR helpers and inline PR qualifiers/destinations share the same
         * region for every rotating base (SDM Vol.1 §5.1.2). */
        bundle(BASE, 0x11, UINT64_C(0x8708040200), add(32,32,33) | 16,
            validator_branch(BASE, BASE)); /* getf.sig r8=f32 */
        restart(v, engine, BASE); v->s.cfm = 8 | (UINT64_C(1) << 14);
        for (unsigned p = 32; p < 128; ++p) v->s.fr[p].significand = p * 123;
        tier0_get_stats(&before);
        for (unsigned fr = 0; fr < 96; ++fr)
            for (unsigned pr = 0; pr < 48; ++pr) {
                v->s.ip = BASE; v->s.slot = 0; v->s.rse_bof = (fr + pr) % 96;
                v->s.cfm = 8 | (UINT64_C(1) << 14) | ((uint64_t)(fr % 8) << 18) |
                    ((uint64_t)fr << 25) | ((uint64_t)pr << 32);
                v->s.pr = random64() | 1;
                frame_compare(v, ref, 3);
            }
        tier0_get_stats(&after); CHECK(after.translations == before.translations + 1);
        /* Reuse native compare destinations when qp aliases rotating p16;
         * .unc must clear both destinations even with a false qualifier. */
        for (unsigned unc = 0; unc < 2; ++unc) {
            uint64_t cmp = unc ? UINT64_C(0x1c1fa141410) : UINT64_C(0x1c1fa140410);
            bundle(BASE, 1, nop, cmp, add(32,32,33) | 63);
            bundle(BASE + 16, 0x11, nop, nop, validator_branch(BASE + 16, BASE));
            restart(v, engine, BASE); tier0_get_stats(&before);
            for (unsigned rotation = 0; rotation < 48; ++rotation)
                for (unsigned trial = 0; trial < 16; ++trial) {
                    v->s.ip = BASE; v->s.slot = 0; v->s.rse_bof = (2 * rotation) % 96;
                    v->s.cfm = 8 | (UINT64_C(1) << 14) | ((uint64_t)(rotation % 8) << 18) |
                        ((uint64_t)rotation << 32);
                    v->s.pr = random64() | 1;
                    ia64_pr_write(&v->s, 16, (trial & 2) != 0);
                    ia64_gr_write(&v->s, 32, 1, (trial & 4) != 0);
                    ia64_gr_write(&v->s, 33, trial & 1, (trial & 8) != 0);
                    frame_compare(v, ref, 6);
                }
            tier0_get_stats(&after); CHECK(after.translations == before.translations + 1);
        }
        /* An interrupted wrapped frame and a memory fault following an inline
         * prefix must expose exactly the physical values and NaTs committed. */
        const unsigned bofs[] = {0,44,90,95};
        for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(bofs); ++n) {
            bundle(BASE, 1, nop, add(32,32,33), add(33,32,33));
            bundle(BASE + 16, 0x11, load(3,8,10), nop, validator_branch(BASE + 16, BASE));
            restart(v, engine, BASE); v->s.rse_bof = bofs[n];
            v->s.cfm = 8 | (UINT64_C(1) << 14) | (UINT64_C(7) << 18);
            ia64_gr_write(&v->s,32,10,false); ia64_gr_write(&v->s,33,20,false);
            v->s.cr[CR_IVA] = BASE; v->s.psr = IA64_PSR_IC | IA64_PSR_I;
            bundle(BASE + 0x3000, 0x11, nop, nop, validator_branch(BASE + 0x3000, BASE + 0x3000));
            frame_compare(v, ref, 1);
            v->timer_armed = true; v->timer_fired = false;
            v->s.cr[CR_ITM] = v->retired + v->clock_offset + 1;
            v->s.cr[CR_ITV] = 150; v->timer_last = v->retired + v->clock_offset;
            frame_compare(v, ref, 3);
            CHECK(v->s.cr[CR_IIP] == BASE);
            restart(v, engine, BASE); v->s.rse_bof = bofs[n];
            v->s.cfm = 8 | (UINT64_C(1) << 14) | (UINT64_C(7) << 18);
            ia64_gr_write(&v->s,32,10,false); ia64_gr_write(&v->s,33,20,false);
            v->s.psr = IA64_PSR_IC; v->s.cr[CR_IVA] = BASE;
            v->s.gr[10] = DATA;
            frame_compare(v, ref, 6); frame_compare(v, ref, 6); /* publish native prefix */
            v->s.nat[10] = true;
            bundle(BASE + 0x5600, 0x11, nop, nop, validator_branch(BASE + 0x5600, BASE + 0x5600));
            frame_compare(v, ref, 4);
            CHECK(v->fault.kind == IA64_FAULT_NAT && v->fault.ip == BASE + 16);
        }
    }
}
/* Incomplete frame (ia64/rse.c ia64_rse_restore/ia64_rse_resume) with
 * frame-relative homes: br.ret leaves the caller's eight locals unloaded
 * (rse_missing = 8, CFLE) in a frame that wraps the physical ring and has
 * rotating registers. The next fetch loads them into the physical slots of
 * the restored BOF, and the caller's region reads them inline; an external
 * interrupt taken before the loads runs its handler in Tier0 with the
 * prefix still missing, and rfi resumes the loads (SDM Vol.2 §6.6). */
static void test_frame_incomplete(struct vcpu *v, struct vcpu *ref)
{
    const uint64_t br_ret = UINT64_C(0x108001100), rfi = UINT64_C(0x08) << 27;
    const gpa_t backing = BASE + 0x6000;
    const unsigned callee_bofs[] = {2, 50};
    for (enum engine_kind engine = ENGINE_TIER0; engine <= ENGINE_LOCKSTEP; ++engine)
      for (unsigned interrupted = 0; interrupted < 2; ++interrupted) {
        code_cache_flush();
        bundle(BASE, 0x11, nop, nop, br_ret);
        bundle(BASE + 0x100, 1, nop, add(40,32,33), add(41,39,34));
        bundle(BASE + 0x110, 0x11, nop, nop, validator_branch(BASE + 0x110, BASE + 0x110));
        bundle(BASE + 0x3000, 1, nop, add(8,16,17), add(9,32,33));
        bundle(BASE + 0x3010, 0x11, nop, nop, rfi);
        const struct ia64_bundle *decoded;
        CHECK(code_get_bundle(v, BASE + 0x3010, &decoded).kind == IA64_STEP_COMPLETE &&
              decoded->insn[2].opcode == IA64_OP_RFI);
        uint64_t values[8];
        for (unsigned i = 0; i < 8; ++i) {
            values[i] = random64(); CHECK(gm_write(backing + 8 * i, &values[i], 8));
        }
        struct tier0_stats before, after; tier0_get_stats(&before);
        for (unsigned n = 0; n < IA64VMM_ARRAY_SIZE(callee_bofs); ++n) {
            restart(v, engine, BASE);
            v->s.psr = IA64_PSR_IC | (interrupted ? IA64_PSR_I : 0); v->s.cr[CR_IVA] = BASE;
            /* Caller CFM: sof 10, sol 8, sor 8, rrb.gr 3, rrb.pr 5. */
            const uint64_t caller = 10 | (UINT64_C(8) << 7) | (UINT64_C(1) << 14) |
                (UINT64_C(3) << 18) | (UINT64_C(5) << 32);
            v->s.ar[AR_PFS] = caller; v->s.br[0] = BASE + 0x100;
            v->s.rse_bof = callee_bofs[n]; v->s.rse_dirty = 0; v->s.cfm = 2;
            v->s.ar[AR_BSP] = v->s.ar[AR_BSPSTORE] = backing + 64;
            v->s.ar[AR_RNAT] = UINT64_C(0x24);  /* slots 2 and 5 hold NaTs */
            for (unsigned p = 16; p < 128; ++p) { v->s.gr[p] = random64(); v->s.nat[p] = false; }
            if (interrupted) {
                v->timer_armed = true; v->timer_fired = false;
                v->s.cr[CR_ITM] = v->retired + v->clock_offset + 3;
                v->s.cr[CR_ITV] = 150; v->timer_last = v->retired + v->clock_offset;
            }
            frame_compare(v, ref, 3);
            unsigned bof = (callee_bofs[n] + 96 - 8) % 96;
            CHECK(v->s.ip == BASE + 0x100 && v->s.rse_missing == 8 && v->s.rse_cfle &&
                  v->s.rse_bof == bof && (v->s.cfm & 127) == 10);
            if (interrupted) {
                frame_compare(v, ref, 3);
                CHECK(v->s.ip == BASE + 0x3010 && v->s.rse_missing == 8 && !v->s.rse_cfle);
                CHECK(v->s.cr[CR_IIP] == BASE + 0x100 && ((v->s.cr[CR_ISR] >> 38) & 1));
                /* The handler read the missing prefix where the interpreter does. */
                unsigned r32 = 32 + (bof + 3) % 96, r33 = 32 + (bof + 4) % 96;
                CHECK(v->s.gr[9] == v->s.gr[r32] + v->s.gr[r33]);
                CHECK(ia64_ivr_read(v) == 150);
                frame_compare(v, ref, 6);
            } else frame_compare(v, ref, 3);
            CHECK(v->s.ip == BASE + 0x110 && !v->s.rse_missing && !v->s.rse_cfle &&
                  v->s.ar[AR_BSPSTORE] == backing);
            for (unsigned k = 0; k < 8; ++k) {
                unsigned slot = (k + 3) % 8, at = 32 + (bof + slot) % 96;
                CHECK(ia64_gr_read(&v->s, 32 + k) == values[slot] && v->s.gr[at] == values[slot]);
                CHECK(ia64_nat_read(&v->s, 32 + k) == (slot == 2 || slot == 5));
            }
            /* r32/r33 are slots 3/4; r39/r34 are slots 2 (NaT) and 5 (NaT). */
            CHECK(ia64_gr_read(&v->s, 40) == values[3] + values[4] && !ia64_nat_read(&v->s, 40));
            CHECK(ia64_nat_read(&v->s, 41));
        }
        tier0_get_stats(&after);
        /* One translation per region entry point for both frame positions:
         * the callee's br.ret, the caller (with its loop bundle), and the
         * handler at its two entry points (budget split before rfi). */
        CHECK(after.translations == before.translations + (interrupted ? 4 : 2));
        CHECK(after.executed_instructions > before.executed_instructions);
      }
}
