/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cfg.h"

/* Defaults match the normal user-config boot entry. Presets retain their raw
 * VMM tokens; labels are only for display. */
const struct cfg_setting cfg_settings[CFG_SETTINGS] = {
    {"engine",
     "Execution engine",
     "interp",
     3,
     {"interp", "tier0", "tier1"},
     {"Interpreter", "Tier 0 JIT", "Tier 1 JIT"}},
    {"code_validity",
     "Code validity checks",
     "compare-always",
     3,
     {"compare-always", "epoch-compare", "none"},
     {"Compare source bundles", "Compare after storage interrupt", "Disabled"}},
    {"reserve_mib",
     "Requested VMM memory (MiB)",
     "128",
     11,
     {"128", "256", "512", "1024", "2048", "3072", "4096", "6144", "8192", "12288", "16384"},
     {"128", "256", "512", "1024", "2048", "3072", "4096", "6144", "8192", "12288", "16384"}},
    {"timeout_sec",
     "Execution timeout (seconds)",
     "0",
     6,
     {"300", "900", "1800", "3600", "7200", "0"},
     {"300", "900", "1800", "3600", "7200", "Unlimited"}},
    {"pause_sec",
     "Checkpoint pause (seconds)",
     "0",
     4,
     {"0", "5", "20", "60"},
     {"Disabled", "5", "20", "60"}},
    {"debug_serial",
     "Serial debug output",
     "off",
     3,
     {"off", "device", "console"},
     {"Disabled", "Serial device", "Firmware console"}},
    {"debug_screen", "Screen debug output", "0", 2, {"0", "1"}, {"Disabled", "Enabled"}},
    {"vcpus",
     "Guest vCPU count",
     "1",
     10,
     {"1", "2", "3", "4", "6", "8", "12", "16", "24", "32"},
     {"1", "2", "3", "4", "6", "8", "12", "16", "24", "32"}},
    {"placement",
     "Host CPU placement",
     "physical-core",
     3,
     {"physical-core", "logical-thread", "core"},
     {"Cores excluding CPU 0 core", "Logical threads", "Cores including CPU 0 sibling"}},
    {"bootmenu", "Boot menu", "1", 2, {"0", "1"}, {"Disabled", "Enabled"}},
    {"log_faults",
     "Fault log limit",
     "0",
     4,
     {"0", "8", "16", "64"},
     {"Disabled", "8", "16", "64"}},
    {"sample_sec",
     "Sampling interval (seconds)",
     "off",
     4,
     {"off", "10", "30", "60"},
     {"Disabled", "10", "30", "60"}},
    {"cpu_model", "CPU model", "montecito", 2, {"montecito", "madison"}, {"Montecito", "Madison"}},
    {"display",
     "Display output",
     "gop",
     2,
     {"gop", "vbios"},
     {"UEFI graphics (GOP)", "Video BIOS"}},
    {"fp_host", "FP acceleration", "1", 2, {"0", "1"}, {"Disabled", "Enabled"}},
    {"fp_experimental", "Experimental FP (unproven)", "0", 2, {"0", "1"}, {"Disabled", "Enabled"}},
    {"fp_approx", "Approximate FP (exp. only)", "0", 2, {"0", "1"}, {"Disabled", "Enabled"}},
    {"fp_check", "Check host FP helpers", "0", 2, {"0", "1"}, {"Disabled", "Enabled"}},
    {"fp_stats", "FP statistics", "0", 2, {"0", "1"}, {"Disabled", "Enabled"}},
    {"iommu",
     "IOMMU DMA protection",
     "auto",
     2,
     {"auto", "off"},
     {"Enabled when available", "Disabled (no DMA protection)"}},
};

static size_t length(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}
static int same(const char *a, size_t an, const char *b, size_t bn)
{
    if (an != bn)
        return 0;
    for (size_t i = 0; i < an; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}
static int blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

unsigned cfg_page_rows(unsigned screen_rows)
{
    return screen_rows > 10 ? screen_rows - 10 : 1;
}
unsigned cfg_page_first(unsigned selected, unsigned visible)
{
    return selected / visible * visible;
}

void cfg_init(struct cfg *c)
{
    for (unsigned i = 0; i < CFG_SETTINGS; ++i)
        for (unsigned k = 0; k < CFG_VALUE_MAX; ++k)
            c->value[i][k] = 0;
    for (unsigned i = 0; i < CFG_SETTINGS; ++i)
        for (size_t k = 0; cfg_settings[i].default_value[k]; ++k)
            c->value[i][k] = cfg_settings[i].default_value[k];
    for (unsigned k = 0; k < CFG_EXTRA_MAX; ++k)
        c->extra[k] = 0;
}

static int add_extra(struct cfg *c, const char *t, size_t n)
{
    size_t used = length(c->extra);
    if (used + (used != 0) + n + 1 > CFG_EXTRA_MAX)
        return 0;
    if (used)
        c->extra[used++] = ' ';
    for (size_t i = 0; i < n; ++i)
        c->extra[used + i] = t[i];
    c->extra[used + n] = 0;
    return 1;
}

int cfg_parse(struct cfg *c, const char *text, size_t len)
{
    int ok = 1;
    unsigned char seen[CFG_SETTINGS] = {0};
    const char *end = text + len, *p = text;
    cfg_init(c);
    for (;;) {
        while (p < end && blank(*p))
            ++p;
        if (p < end && *p == '#') {
            while (p < end && *p != '\n')
                ++p;
            continue;
        }
        if (p >= end)
            break;
        const char *t = p;
        while (p < end && !blank(*p))
            ++p;
        size_t n = (size_t)(p - t), key = 0;
        while (key < n && t[key] != '=')
            ++key;
        int placed = 0;
        if (key < n)
            for (unsigned i = 0; i < CFG_SETTINGS && !placed; ++i)
                if (same(t, key, cfg_settings[i].key, length(cfg_settings[i].key)) && !seen[i]) {
                    seen[i] = 1;
                    size_t value_len = n - key - 1;
                    if (value_len >= CFG_VALUE_MAX) {
                        ok = 0;
                        break;
                    }
                    for (unsigned k = 0; k < CFG_VALUE_MAX; ++k)
                        c->value[i][k] = 0;
                    for (size_t k = 0; k < value_len; ++k)
                        c->value[i][k] = t[key + 1 + k];
                    placed = 1;
                }
        if (!placed && !add_extra(c, t, n))
            ok = 0;
    }
    return ok;
}

void cfg_cycle(struct cfg *c, unsigned i, int direction)
{
    const struct cfg_setting *s = &cfg_settings[i];
    unsigned pos = 0;
    while (pos < s->nchoices &&
           !same(c->value[i], length(c->value[i]), s->choices[pos], length(s->choices[pos])))
        ++pos;
    if (direction > 0)
        pos = pos + 1 < s->nchoices ? pos + 1 : 0;
    else
        pos = pos ? pos - 1 : s->nchoices - 1;
    /* Every table row has at least two choices. */
    const char *v = s->choices[pos]; /* NOLINT(clang-analyzer-security.ArrayBound) */
    for (unsigned k = 0; k < CFG_VALUE_MAX; ++k)
        c->value[i][k] = 0;
    for (size_t k = 0; v[k]; ++k)
        c->value[i][k] = v[k];
}

const char *cfg_value_text(const struct cfg *c, unsigned i)
{
    const struct cfg_setting *s = &cfg_settings[i];
    for (unsigned k = 0; k < s->nchoices; ++k)
        if (same(c->value[i], length(c->value[i]), s->choices[k], length(s->choices[k])))
            return s->choice_labels[k];
    return c->value[i][0] ? c->value[i] : "Empty (invalid)";
}

int cfg_extra_put(struct cfg *c, char ch)
{
    size_t n = length(c->extra);
    if (ch < 0x20 || ch > 0x7e || n + 1 >= CFG_EXTRA_MAX)
        return 0;
    c->extra[n] = ch;
    c->extra[n + 1] = 0;
    return 1;
}

void cfg_extra_backspace(struct cfg *c)
{
    size_t n = length(c->extra);
    if (n)
        c->extra[n - 1] = 0;
}

/* The tokens in order (settings, then extra) joined by separator; *ok is 0
 * when cap was too small (the text is then incomplete). */
static size_t join(const struct cfg *c, char *out, size_t cap, char separator, size_t *ok)
{
    size_t used = 0;
    *ok = 1;
#define PUT(ch)                                                                                    \
    do {                                                                                           \
        char put_ = (ch);                                                                          \
        if (used + 1 >= cap)                                                                       \
            *ok = 0;                                                                               \
        else                                                                                       \
            out[used] = put_;                                                                      \
        ++used;                                                                                    \
    } while (0)
    for (unsigned i = 0; i <= CFG_SETTINGS; ++i) {
        const char *src;
        size_t n;
        if (i < CFG_SETTINGS) {
            if (used)
                PUT(separator);
            for (const char *k = cfg_settings[i].key; *k; ++k)
                PUT(*k);
            PUT('=');
            for (const char *v = c->value[i]; *v; ++v)
                PUT(*v);
            continue;
        }
        src = c->extra;
        n = length(src);
        for (size_t at = 0; at < n;) {
            while (at < n && blank(src[at]))
                ++at;
            if (at >= n)
                break;
            if (used)
                PUT(separator);
            while (at < n && !blank(src[at]))
                PUT(src[at++]);
        }
    }
#undef PUT
    if (*ok && used < cap)
        out[used] = 0;
    return used;
}

size_t cfg_tokens(const struct cfg *c, char *out, size_t cap)
{
    size_t ok, n;
    if (!cap)
        return (size_t)-1;
    n = join(c, out, cap, ' ', &ok);
    if (!ok) {
        out[0] = 0;
        return (size_t)-1;
    }
    return n;
}

static const char header[] =
    "# VMM options: one key=value per line; lines starting with # are comments.\n"
    "# Managed settings are explicit; the editor fills absent keys with normal boot defaults.\n"
    "# Safe mode keeps vmm_media and guest_media.\n";

size_t cfg_serialize(const struct cfg *c, char *out, size_t cap)
{
    char tokens[CFG_TOKENS_MAX + 1];
    size_t n = cfg_tokens(c, tokens, sizeof(tokens)), used = 0;
    if (n == (size_t)-1)
        return 0;
    if (sizeof(header) - 1 + n + 2 > cap)
        return 0;
    for (size_t i = 0; i < sizeof(header) - 1; ++i)
        out[used++] = header[i];
    for (size_t i = 0; i < n; ++i)
        out[used++] = (char)(tokens[i] == ' ' ? '\n' : tokens[i]);
    if (n)
        out[used++] = '\n';
    out[used] = 0;
    return used;
}
