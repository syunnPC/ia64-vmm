/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "core/log_internal.h"

static unsigned fb_calls;
static char fb_text[4096];
static size_t fb_used;
static bool nested;

static void fb_sink(const char *s, size_t n)
{
    ++fb_calls;
    CHECK(fb_used + n < sizeof(fb_text));
    memcpy(fb_text + fb_used, s, n);
    fb_used += n;
    fb_text[fb_used] = 0;
    if (nested) {
        nested = false;
        LOGE("nested panic diagnostic %d", 7);
    }
}

static void expect_bytes(uint64_t start, const char *expected, size_t n)
{
    const struct log_ring *r = log_get_ring();
    CHECK(r->write_pos == start + n);
    for (size_t i = 0; i < n; ++i)
        CHECK(r->data[(start + i) % LOG_RING_SIZE] == expected[i]);
}

static void expect_text(uint64_t start, const char *expected)
{
    expect_bytes(start, expected, strlen(expected));
}

void test_log(void)
{
    const struct log_ring *r = log_get_ring();
    CHECK(r->magic == LOG_RING_MAGIC && r->generation != 0);
    CHECK(r->flushed_pos == 0);
    uint64_t start = r->write_pos;
    LOGI("hello %d", 12);
    expect_text(start, "[info] hello 12\n");
    start = r->write_pos;
    LOGD("hidden");
    expect_text(start, "[debug] hidden\n");
    start = r->write_pos;
    log_set_level(LOG_DEBUG);
    LOGD("visible");
    expect_text(start, "[debug] visible\n");
    log_set_level(LOG_INFO);
    start = r->write_pos;
    LOGW("warning");
    expect_text(start, "[warn] warning\n");
    start = r->write_pos;
    LOGE("error");
    expect_text(start, "[error] error\n");

    log_set_fb_sink(fb_sink);
    LOGD("retained without framebuffer mirror");
    CHECK(fb_calls == 0);
    start = r->write_pos;
    vmm_log_guest_text("hel\r", 4);
    CHECK(r->write_pos == start && fb_calls == 0);
    vmm_log_guest_text("lo\r\nnext", 8);
    expect_text(start, "[guest] hello\n");
    CHECK(fb_calls == 1 && !strcmp(fb_text, "[guest] hello\n"));
    start = r->write_pos;
    log_flush_guest_text();
    expect_text(start, "[guest] next\n");
    CHECK(fb_calls == 2);
    start = r->write_pos;
    log_flush_guest_text();
    CHECK(r->write_pos == start);
    vmm_log_guest_text("\n\n", 2);
    expect_text(start, "[guest] \n[guest] \n");
    vmm_log_fb_enable(false);
    unsigned calls = fb_calls;
    LOGI("framebuffer disabled");
    CHECK(fb_calls == calls);
    vmm_log_fb_enable(true);
    nested = true;
    start = r->write_pos;
    LOGE("outer panic diagnostic");
    expect_text(start, "[error] outer panic diagnostic\n[error] nested panic diagnostic 7\n");
    CHECK(fb_calls == calls + 1);
    log_set_fb_sink(NULL);

    char long_text[4096];
    memset(long_text, 'T', sizeof(long_text));
    long_text[sizeof(long_text) - 1] = 0;
    start = r->write_pos;
    LOGI("%s", long_text);
    CHECK(r->write_pos - start == 2047);
    CHECK(r->data[(r->write_pos - 1) % LOG_RING_SIZE] == '\n');
    start = r->write_pos;
    vmm_log_guest_text(long_text, 2048);
    CHECK(r->write_pos == start);
    vmm_log_guest_text("\n", 1);
    CHECK(r->write_pos - start == 8 + 2048 + 1);
    static char oversized[LOG_RING_SIZE + 1];
    memset(oversized, 'G', sizeof(oversized));
    start = r->write_pos;
    vmm_log_guest_text(oversized, sizeof(oversized));
    CHECK(r->write_pos == start);
    log_flush_guest_text();
    CHECK(r->write_pos - start == LOG_RING_SIZE);
    const char *truncated = " [truncated]\n";
    size_t suffix = strlen(truncated);
    for (size_t i = 0; i < suffix; ++i)
        CHECK(r->data[(r->write_pos - suffix + i) % LOG_RING_SIZE] == truncated[i]);

    char payload[1001];
    memset(payload, 'w', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = 0;
    const size_t record_size = 7 + sizeof(payload) - 1 + 1;
    start = r->write_pos;
    unsigned records = LOG_RING_SIZE / record_size + 5;
    for (unsigned i = 0; i < records; ++i)
        LOGI("%s", payload);
    CHECK(r->write_pos == start + records * record_size);
    const char *tag = "[info] ";
    /* Check every retained byte, including a partial overwritten record. */
    for (uint64_t pos = r->write_pos - LOG_RING_SIZE; pos < r->write_pos; ++pos) {
        size_t offset = (pos - start) % record_size;
        char expected = offset < 7 ? tag[offset] : offset == record_size - 1 ? '\n' : 'w';
        CHECK(r->data[pos % LOG_RING_SIZE] == expected);
    }
    CHECK(r->flushed_pos == 0);
}

void test_guest_serial(void)
{
    static const char raw[] = {'A', '\r', '\n', 0, (char)0xff, '\033', '[', 'm'};
    char out[sizeof(raw)];
    uint64_t cursor = 0, lost;
    vmm_guest_serial_configure(true, false);
    CHECK(vmm_guest_serial_screen_enabled());
    vmm_log_fb_enable(false);
    log_set_fb_sink(fb_sink);
    fb_used = fb_calls = 0;
    vmm_guest_serial_write(raw, sizeof(raw));
    log_drain();
    CHECK(fb_used == sizeof(raw) && !memcmp(fb_text, raw, sizeof(raw)));
    CHECK(vmm_guest_serial_read(&cursor, out, 3, &lost) == 3 && lost == 0);
    CHECK(!memcmp(out, raw, 3));
    CHECK(vmm_guest_serial_read(&cursor, out + 3, sizeof(out) - 3, &lost) == sizeof(out) - 3);
    CHECK(!memcmp(out, raw, sizeof(raw)));
    CHECK(vmm_guest_serial_read(&cursor, out, sizeof(out), &lost) == 0);
    log_set_fb_sink(NULL);
    log_flush_guest_text();
    vmm_guest_serial_configure(true, true);
    CHECK(!vmm_guest_serial_screen_enabled());
    /* An overrun retains the exact suffix and reports its missing prefix. */
    static char flood[LOG_RING_SIZE + 17];
    for (size_t i = 0; i < sizeof(flood); ++i)
        flood[i] = (char)i;
    vmm_guest_serial_write(flood, sizeof(flood));
    cursor = 0;
    CHECK(vmm_guest_serial_read(&cursor, out, sizeof(out), &lost) == sizeof(out));
    CHECK(lost == 17 && !memcmp(out, flood + 17, sizeof(out)));
    vmm_guest_serial_configure(false, false);
    log_flush_guest_text();
    vmm_log_fb_enable(true);
}

static void guest_text(const char *s)
{
    vmm_log_guest_text(s, strlen(s));
}

/* Final-summary collector: prefix match on whole ring lines,
 * newest max kept in order, truncation, wrapped-ring start fragment. */
void test_log_matching(void)
{
    const struct log_ring *r = log_get_ring();
    static char lines[3][LOG_MATCH_COLUMNS + 1];
    size_t kept = 99;
    log_flush_guest_text();
    CHECK(log_ring_matching(r, "[guest] d2t: ", lines, 3, &kept) == 0 && kept == 0);
    guest_text("d2t: one\nother\nd2t:two\nd2t: two\n");
    LOGI("d2t: not a guest line");
    CHECK(log_ring_matching(r, "[guest] d2t: ", lines, 3, &kept) == 2 && kept == 2);
    CHECK(!strcmp(lines[0], "[guest] d2t: one") && !strcmp(lines[1], "[guest] d2t: two"));
    /* A one-character prefix change finds nothing: matching is exact. */
    CHECK(log_ring_matching(r, "[guest] d2u: ", lines, 3, &kept) == 0 && kept == 0);
    guest_text("d2t: three\nunrelated [guest] d2t: x\nd2t: four\n");
    CHECK(log_ring_matching(r, "[guest] d2t: ", lines, 3, &kept) == 4 && kept == 3);
    CHECK(!strcmp(lines[0], "[guest] d2t: two") && !strcmp(lines[1], "[guest] d2t: three") &&
          !strcmp(lines[2], "[guest] d2t: four"));
    char wide[400];
    memset(wide, 'L', sizeof(wide));
    memcpy(wide, "d2t: ", 5);
    wide[sizeof(wide) - 2] = '\n';
    wide[sizeof(wide) - 1] = 0;
    guest_text(wide);
    guest_text("d2t: five\n");
    CHECK(log_ring_matching(r, "[guest] d2t: ", lines, 3, &kept) == 6 && kept == 3);
    CHECK(!strcmp(lines[0], "[guest] d2t: four") && strlen(lines[1]) == LOG_MATCH_COLUMNS &&
          lines[1][LOG_MATCH_COLUMNS - 1] == 'L' && !strcmp(lines[2], "[guest] d2t: five"));
    /* max 1 keeps only the newest; a wrapped ring drops the old ones. */
    CHECK(log_ring_matching(r, "[guest] d2t: ", lines, 1, &kept) == 6 && kept == 1 &&
          !strcmp(lines[0], "[guest] d2t: five"));
    static char flood[LOG_RING_SIZE];
    memset(flood, 'f', sizeof(flood));
    flood[sizeof(flood) - 1] = '\n';
    vmm_log_guest_text(flood, sizeof(flood));
    CHECK(log_ring_matching(r, "[guest] d2t: ", lines, 3, &kept) == 0 && kept == 0);
}
