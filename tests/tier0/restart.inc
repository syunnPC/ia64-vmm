/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The upper-tier hotness schedule restarts when a link dies
 * (tier0.h step 4). A fake upper tier counts what Tier0 asks of it on a
 * one-bundle self loop (3 instructions per lookup, so v->retired / 3 is the
 * number of lookups run). No timing: only counts of Tier0 lookups.
 *
 * The head is promoted at `threshold` lookups, runs linked for DEATH_AT
 * lookups, then its link dies, once by enter (leaves *link == 0) and once by
 * a re-probe after the sequence moved (probe returns 0). From that moment
 * hot() must be called at threshold, threshold << 1, ... lookups, counted
 * from the death, and a head the upper tier holds for REJECTS calls is
 * linked again at threshold << REJECTS lookups after the death. Before the
 * restart the calls came at the next powers of two of the region's whole
 * life (here 128, 256, ... lookups, i.e. 28 after the death, not 4).
 */
#define RS_THRESHOLD 4u
#define RS_DEATH_AT 100u
#define RS_REJECTS 3u
#define RS_MAX_CALLS 16u
static struct {
    bool alive, kill_by_probe, died;
    uint64_t sequence, link, enters, death_retired;
    unsigned calls, rejects_left;
    uint64_t call_at[RS_MAX_CALLS], relinked_at;
} rs;
static uint64_t rs_probe(struct vcpu *v, gpa_t pa)
{
    (void)v; (void)pa;
    if (!rs.alive && rs.link && !rs.died) { rs.died = true; rs.death_retired = v->retired; }
    return rs.alive ? rs.link : 0;
}
static enum tier0_upper_status rs_enter(struct vcpu *v, uint64_t *link, uint64_t budget, struct vcpu_exit *out)
{
    (void)budget; (void)out;
    if (++rs.enters == RS_DEATH_AT) {
        rs.alive = false;
        if (rs.kill_by_probe) ++rs.sequence;       /* the next lookup re-probes */
        else { *link = 0; rs.died = true; rs.death_retired = v->retired; }
    }
    return TIER0_UPPER_DECLINED;
}
static enum tier0_upper_hot rs_hot(struct vcpu *v, gpa_t pa, bool loop_head)
{
    (void)pa; (void)loop_head;
    CHECK(rs.calls < RS_MAX_CALLS);
    rs.call_at[rs.calls++] = v->retired;
    if (rs.died && rs.rejects_left) { --rs.rejects_left; return TIER0_HOT_REJECTED; }
    if (rs.died) rs.relinked_at = v->retired;
    rs.alive = true; rs.link = 1; ++rs.sequence;
    return TIER0_HOT_LINKED;
}
static void rs_reclaim(void) {}
static const unsigned rs_due = 1;
static struct tier0_upper_ops rs_ops = {.probe = rs_probe, .enter = rs_enter, .hot = rs_hot, .reclaim = rs_reclaim,
                                        .sequence = &rs.sequence, .loop_threshold = RS_THRESHOLD,
                                        .trace_threshold = RS_THRESHOLD, .heads = &rs.sequence, .head_mask = 0,
                                        .reclaim_due = &rs_due};
static void test_hotness_restart(struct vcpu *v, bool kill_by_probe)
{
    memset(&rs, 0, sizeof(rs));
    rs.kill_by_probe = kill_by_probe;
    rs.rejects_left = RS_REJECTS;
    code_cache_flush();
    bundle(BASE, 0x11, nop, nop, validator_branch(BASE, BASE));
    restart(v, ENGINE_TIER0, BASE);
    tier0_set_upper(&rs_ops);
    CHECK(engine_run(v, 3 * (RS_DEATH_AT + 100)).reason == VCPU_EXIT_BUDGET);
    tier0_set_upper(NULL);
    CHECK(rs.died && rs.alive && rs.enters > RS_DEATH_AT);
    /* The first promotion, before any death: entries == threshold. */
    CHECK(rs.call_at[0] / 3 == RS_THRESHOLD - 1 || rs.call_at[0] / 3 == RS_THRESHOLD);
    /* After the death: calls at T, 2T, 4T, ... lookups from the death; the
     * last of RS_REJECTS + 1 calls links the head again. */
    unsigned first = 1;
    CHECK(rs.calls == first + RS_REJECTS + 1);
    for (unsigned k = 0; k <= RS_REJECTS; ++k)
        CHECK((rs.call_at[first + k] - rs.death_retired) / 3 == (uint64_t)RS_THRESHOLD << k);
    CHECK((rs.relinked_at - rs.death_retired) / 3 == (uint64_t)RS_THRESHOLD << RS_REJECTS);
    code_cache_flush();
}
