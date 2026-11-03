/* SPDX-License-Identifier: GPL-2.0-or-later */
/* include/ia64vmm/host_wake.h against Intel SDM Vol.2 MONITOR/MWAIT,
 * Vol.1 CPUID leaf 05H and Vol.3 §11.10.5 (monitor line sizes), and the
 * host mailbox protocol. */
#include "test.h"
#include "ia64vmm/host_wake.h"

static uint32_t max_leaf, leaf1_ecx, leaf5[4];
static unsigned leaf5_reads;
static void fake_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b, uint32_t *c,
                       uint32_t *d)
{
    CHECK(!subleaf);
    *a = *b = *c = *d = 0;
    CHECK(leaf <= max_leaf);
    if (leaf == 0)
        *a = max_leaf;
    else if (leaf == 1)
        *c = leaf1_ecx;
    else if (leaf == 5) {
        *a = leaf5[0];
        *b = leaf5[1];
        *c = leaf5[2];
        *d = leaf5[3];
        ++leaf5_reads;
    } else
        CHECK(false);
}
static bool usable(uint32_t max, uint32_t ecx1, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    max_leaf = max;
    leaf1_ecx = ecx1;
    leaf5[0] = a;
    leaf5[1] = b;
    leaf5[2] = c;
    leaf5[3] = d;
    const char *why = host_mwait_unusable(fake_cpuid);
    CHECK(!why || *why);
    return !why;
}
#define MONITOR (1u << 3)

static void cpuid_rules(void)
{
    /* Leaf 05H as Sandy Bridge and Zen 2 report it: 64-byte lines,
     * extensions and interrupt break events, C1 sub-states. */
    CHECK(usable(0xd, MONITOR, 0x40, 0x40, 0x3, 0x00001120));
    CHECK(usable(0x10, MONITOR, 0x40, 0x40, 0x3, 0x11));
    /* CPUID.01H:ECX.MONITOR[3] = 0: no MONITOR, so no address-range wait;
     * leaf 05H is not consulted. */
    leaf5_reads = 0;
    CHECK(!usable(0xd, 0, 0x40, 0x40, 0x3, 0x11));
    CHECK(!usable(0xd, ~MONITOR, 0x40, 0x40, 0x3, 0x11));
    CHECK(!leaf5_reads);
    /* Leaf 05H is valid only if MAX_LEAF >= 05H. */
    CHECK(!usable(4, MONITOR, 0x40, 0x40, 0x3, 0x11));
    CHECK(!usable(0, MONITOR, 0x40, 0x40, 0x3, 0x11));
    CHECK(!leaf5_reads);
    /* Without enumerated extensions, MWAIT takes EAX = ECX = 0 only, which
     * is the form used. */
    CHECK(usable(5, MONITOR, 0x40, 0x40, 0, 0));
    /* With extensions, EAX = 0 names C1 sub-state 0: EDX[7:4] must count
     * at least one C1 sub-state. C0 and deeper sub-states do not help. */
    CHECK(!usable(5, MONITOR, 0x40, 0x40, 0x1, 0x0));
    CHECK(!usable(5, MONITOR, 0x40, 0x40, 0x3, 0x2202));
    CHECK(usable(5, MONITOR, 0x40, 0x40, 0x1, 0x10));
    /* The mailbox word must fit in the smallest monitor line; the padded
     * mailbox line must cover the largest (false wake-ups). */
    CHECK(!usable(5, MONITOR, 0, 0, 0x3, 0x11));
    CHECK(!usable(5, MONITOR, 2, 0x40, 0x3, 0x11));
    CHECK(usable(5, MONITOR, 0x10, 0x40, 0x3, 0x11));
    CHECK(usable(5, MONITOR, 0x40, HOST_MAILBOX_LINE, 0x3, 0x11));
    CHECK(!usable(5, MONITOR, 0x40, 0x80, 0x3, 0x11));
    CHECK(!usable(5, MONITOR, 0x40, 0x20, 0x3, 0x11));
    /* Reserved upper halves of EAX/EBX are not sizes. */
    CHECK(usable(5, MONITOR, 0xffff0040, 0xffff0040, 0x3, 0x11));
    CHECK(sizeof(struct host_mailbox) == HOST_MAILBOX_LINE &&
          _Alignof(struct host_mailbox) == HOST_MAILBOX_LINE);
}

/* The owner's arm step: records what it saw and may post as a peer would
 * between arming and the final check. */
static struct host_mailbox box;
static unsigned arms, idle_at_arm, post_at_arm;
static const unsigned *armed;
static void fake_arm(const unsigned *line)
{
    ++arms;
    armed = line;
    idle_at_arm = __atomic_load_n(&box.idle, __ATOMIC_SEQ_CST);
    if (post_at_arm)
        CHECK(!host_mailbox_post(&box, post_at_arm));
}
static unsigned take(void)
{
    return __atomic_exchange_n(&box.requests, 0, __ATOMIC_SEQ_CST);
}

static void protocol(void)
{
    /* A running owner is never interrupted: it sees the bits at its next
     * poll or idle check. */
    CHECK(box.idle == HOST_IDLE_RUNNING);
    CHECK(!host_mailbox_post(&box, 1));
    CHECK(take() == 1);
    /* A halted owner is interrupted for a new bit only. */
    CHECK(host_idle_enter(&box, HOST_IDLE_HALT, fake_arm));
    CHECK(box.idle == HOST_IDLE_HALT && !arms);
    CHECK(host_mailbox_post(&box, 1));
    CHECK(!host_mailbox_post(&box, 1));
    CHECK(host_mailbox_post(&box, 1 | 4));
    CHECK(!host_mailbox_post(&box, 4));
    host_idle_leave(&box);
    CHECK(box.idle == HOST_IDLE_RUNNING && take() == 5);
    /* A pending bit keeps the owner awake. */
    CHECK(!host_mailbox_post(&box, 2));
    CHECK(!host_idle_enter(&box, HOST_IDLE_HALT, fake_arm));
    host_idle_leave(&box);
    CHECK(!host_idle_enter(&box, HOST_IDLE_MONITOR, fake_arm));
    host_idle_leave(&box);
    CHECK(take() == 2);
    /* A monitoring owner arms its mailbox word after publishing its state
     * and before its final check; its post is a plain store. */
    arms = 0;
    CHECK(host_idle_enter(&box, HOST_IDLE_MONITOR, fake_arm));
    CHECK(arms == 1 && armed == &box.requests && idle_at_arm == HOST_IDLE_MONITOR);
    CHECK(!host_mailbox_post(&box, 8));
    host_idle_leave(&box);
    CHECK(take() == 8);
    /* A store between arming and the check is seen by the check. */
    post_at_arm = 16;
    CHECK(!host_idle_enter(&box, HOST_IDLE_MONITOR, fake_arm));
    host_idle_leave(&box);
    post_at_arm = 0;
    CHECK(take() == 16 && box.idle == HOST_IDLE_RUNNING);
}

void test_host_wake(void)
{
    cpuid_rules();
    protocol();
}
