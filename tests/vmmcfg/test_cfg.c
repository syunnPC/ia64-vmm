/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tools/vmmcfg/cfg.h"

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                 \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static unsigned index_of(const char *key)
{
    for (unsigned i = 0; i < CFG_SETTINGS; ++i)
        if (!strcmp(cfg_settings[i].key, key))
            return i;
    printf("FAIL no setting %s\n", key);
    exit(1);
}
static int parse(struct cfg *c, const char *text)
{
    return cfg_parse(c, text, strlen(text));
}
static const char *tokens(const struct cfg *c)
{
    static char out[CFG_TOKENS_MAX + 1];
    CHECK(cfg_tokens(c, out, sizeof(out)) != (size_t)-1);
    return out;
}
static void round_trip(const struct cfg *c)
{
    struct cfg d;
    char text[2048], again[2048];
    CHECK(cfg_serialize(c, text, sizeof(text)) > 0);
    CHECK(parse(&d, text));
    CHECK(!memcmp(c, &d, sizeof(*c)));
    CHECK(cfg_serialize(&d, again, sizeof(again)) == strlen(text));
    CHECK(!strcmp(again, text));
}

static void reserve_presets(void)
{
    static const char *values[] = {"128",  "256",  "512",  "1024",  "2048", "3072",
                                   "4096", "6144", "8192", "12288", "16384"};
    unsigned r = index_of("reserve_mib"), count = sizeof(values) / sizeof(values[0]);
    struct cfg c;
    char token[64], text[2048];
    CHECK(cfg_settings[r].nchoices == count);
    cfg_init(&c);
    for (unsigned i = 0; i < count; ++i) {
        CHECK(!strcmp(c.value[r], values[i]));
        CHECK(!strcmp(cfg_value_text(&c, r), values[i]));
        round_trip(&c);
        cfg_cycle(&c, r, 1);
    }
    CHECK(!strcmp(c.value[r], values[0]));
    for (unsigned i = count; i; --i) {
        cfg_cycle(&c, r, -1);
        CHECK(!strcmp(c.value[r], values[i - 1]));
    }
    for (unsigned i = 0; i < count; ++i) {
        snprintf(token, sizeof(token), "reserve_mib=%s", values[i]);
        CHECK(parse(&c, token));
        CHECK(!strcmp(c.value[r], values[i]));
        CHECK(cfg_serialize(&c, text, sizeof(text)) > 0);
        strcat(token, "\n");
        CHECK(strstr(text, token));
        round_trip(&c);
    }
    CHECK(parse(&c, "reserve_mib=1536"));
    CHECK(!strcmp(cfg_value_text(&c, r), "1536"));
    round_trip(&c);
    cfg_cycle(&c, r, -1);
    CHECK(!strcmp(c.value[r], "16384"));
}

static void vcpu_settings(void)
{
    static const char *values[] = {"1", "2", "3", "4", "6", "8", "12", "16", "24", "32"};
    unsigned v = index_of("vcpus"), p = index_of("placement");
    struct cfg c;
    char text[2048];
    cfg_init(&c);
    CHECK(!strcmp(c.value[v], "1"));
    CHECK(cfg_settings[v].nchoices == sizeof(values) / sizeof(values[0]));
    for (unsigned i = 0; i < cfg_settings[v].nchoices; ++i) {
        CHECK(!strcmp(c.value[v], values[i]));
        CHECK(!strcmp(c.value[p], "physical-core"));
        round_trip(&c);
        cfg_cycle(&c, v, 1);
    }
    CHECK(!strcmp(c.value[v], "1"));

    /* Counts saved as extra tokens by older editors become a single setting. */
    CHECK(parse(&c, "# Saved options\nengine=tier1\nplacement=physical-core\n"
                    "vmm_media=00:03.0 vcpus=5 smp_serialize=1\n"));
    CHECK(!strcmp(c.value[v], "5"));
    CHECK(!strcmp(cfg_value_text(&c, v), "5"));
    CHECK(!strcmp(c.extra, "vmm_media=00:03.0 smp_serialize=1"));
    for (unsigned i = 0; i < cfg_settings[p].nchoices; ++i) {
        cfg_cycle(&c, p, 1);
        CHECK(!strcmp(c.value[v], "5"));
        CHECK(cfg_serialize(&c, text, sizeof(text)) > 0);
        const char *count = strstr(text, "vcpus=");
        CHECK(count && !strncmp(count, "vcpus=5\n", 8));
        CHECK(!strstr(count + 6, "vcpus="));
        round_trip(&c);
    }
    CHECK(!strcmp(c.value[p], "physical-core"));
    cfg_cycle(&c, v, 1);
    CHECK(!strcmp(c.value[v], "1"));
    CHECK(!strcmp(c.value[p], "physical-core"));
    round_trip(&c);

    CHECK(parse(&c, "vcpus=1 placement=logical-thread"));
    cfg_cycle(&c, p, 1);
    CHECK(!strcmp(c.value[v], "1"));
    round_trip(&c);

    CHECK(parse(&c, "placement=logical-thread\nvmm_media=00:03.0 vcpus=7\n"));
    CHECK(!strcmp(c.value[v], "7"));
    CHECK(!strcmp(c.extra, "vmm_media=00:03.0"));
    cfg_cycle(&c, p, 1);
    CHECK(!strcmp(c.value[v], "7"));
    round_trip(&c);
}

int main(void)
{
    struct cfg c;
    char text[2048];
    static const struct {
        const char *key, *value;
    } defaults[] = {
        {"engine", "interp"},
        {"code_validity", "compare-always"},
        {"reserve_mib", "128"},
        {"timeout_sec", "0"},
        {"pause_sec", "0"},
        {"debug_serial", "off"},
        {"debug_screen", "0"},
        {"vcpus", "1"},
        {"placement", "physical-core"},
        {"bootmenu", "1"},
        {"log_faults", "0"},
        {"sample_sec", "off"},
        {"cpu_model", "montecito"},
        {"display", "gop"},
        {"fp_host", "1"},
        {"fp_experimental", "0"},
        {"fp_approx", "0"},
        {"fp_check", "0"},
        {"fp_stats", "0"},
        {"iommu", "auto"},
    };
    static const char default_tokens[] =
        "engine=interp code_validity=compare-always reserve_mib=128 timeout_sec=0 "
        "pause_sec=0 debug_serial=off debug_screen=0 vcpus=1 placement=physical-core "
        "bootmenu=1 log_faults=0 sample_sec=off cpu_model=montecito display=gop "
        "fp_host=1 fp_experimental=0 fp_approx=0 fp_check=0 fp_stats=0 iommu=auto";

    vcpu_settings();

    /* An absent or empty file has the normal user-config boot defaults. */
    CHECK(CFG_SETTINGS == sizeof(defaults) / sizeof(defaults[0]));
    cfg_init(&c);
    for (unsigned i = 0; i < CFG_SETTINGS; ++i)
        CHECK(!strcmp(c.value[index_of(defaults[i].key)], defaults[i].value));
    CHECK(!strcmp(tokens(&c), default_tokens));
    CHECK(parse(&c, ""));
    CHECK(!strcmp(tokens(&c), default_tokens));
    round_trip(&c);

    CHECK(parse(&c, "iommu=off"));
    CHECK(!strcmp(c.value[index_of("iommu")], "off") && !c.extra[0]);
    CHECK(!strcmp(cfg_value_text(&c, index_of("iommu")), "Disabled (no DMA protection)"));
    round_trip(&c);
    cfg_cycle(&c, index_of("iommu"), 1);
    CHECK(!strcmp(c.value[index_of("iommu")], "auto"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("iommu")), "Enabled when available"));

    /* Labels describe the values while serialization retains the raw tokens. */
    CHECK(!strcmp(cfg_settings[index_of("reserve_mib")].label, "Requested VMM memory (MiB)"));
    reserve_presets();
    CHECK(!strcmp(cfg_settings[index_of("cpu_model")].label, "CPU model"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("timeout_sec")), "Unlimited"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("pause_sec")), "Disabled"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("log_faults")), "Disabled"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("sample_sec")), "Disabled"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("debug_serial")), "Disabled"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("engine")), "Interpreter"));
    {
        static const char *booleans[] = {"debug_screen", "bootmenu", "fp_host", "fp_experimental",
                                         "fp_approx",    "fp_check", "fp_stats"};
        for (unsigned i = 0; i < sizeof(booleans) / sizeof(booleans[0]); ++i) {
            unsigned b = index_of(booleans[i]);
            const char *enabled = !strcmp(c.value[b], "1") ? "Enabled" : "Disabled";
            CHECK(!strcmp(cfg_value_text(&c, b), enabled));
            cfg_cycle(&c, b, 1);
            CHECK(!strcmp(cfg_value_text(&c, b),
                          !strcmp(enabled, "Enabled") ? "Disabled" : "Enabled"));
        }
    }

    /* Cycling wraps through concrete choices only, including every default. */
    for (unsigned i = 0; i < CFG_SETTINGS; ++i) {
        const struct cfg_setting *s = &cfg_settings[i];
        cfg_init(&c);
        CHECK(s->nchoices >= 2 && s->nchoices <= CFG_MAX_CHOICES);
        unsigned initial = s->nchoices;
        for (unsigned k = 0; k < s->nchoices; ++k) {
            CHECK(s->choices[k][0] && s->choice_labels[k][0]);
            if (!strcmp(c.value[i], s->choices[k]))
                initial = k;
        }
        CHECK(initial < s->nchoices);
        for (unsigned k = 1; k <= s->nchoices; ++k) {
            cfg_cycle(&c, i, 1);
            unsigned at = (initial + k) % s->nchoices;
            CHECK(!strcmp(c.value[i], s->choices[at]));
            CHECK(!strcmp(cfg_value_text(&c, i), s->choice_labels[at]));
        }
        cfg_cycle(&c, i, -1);
        CHECK(!strcmp(c.value[i], s->choices[(initial + s->nchoices - 1) % s->nchoices]));
        round_trip(&c);
    }

    /* Comments, CRLF, several tokens per line, tabs, and advanced tokens. */
    CHECK(parse(
        &c,
        "# header\r\n\r\nengine=tier1 pause_sec=5\r\n\t# note\r\nselftest=host\r\n  display=vbios\tfoo\r\nvmm_media=00:03.0"));
    CHECK(!strcmp(c.value[index_of("engine")], "tier1"));
    CHECK(!strcmp(c.value[index_of("pause_sec")], "5"));
    CHECK(!strcmp(c.value[index_of("display")], "vbios"));
    CHECK(!strcmp(c.value[index_of("timeout_sec")], "0"));
    CHECK(!strcmp(c.extra, "selftest=host foo vmm_media=00:03.0"));
    CHECK(strstr(tokens(&c), "selftest=host foo vmm_media=00:03.0"));
    round_trip(&c);

    /* The first managed value and all duplicate tokens survive in order. */
    CHECK(parse(&c, "cpu_model=madison cpu_model=montecito unknown=1 engine=tier0 engine=interp"));
    CHECK(!strcmp(c.value[index_of("cpu_model")], "madison"));
    CHECK(!strcmp(c.value[index_of("engine")], "tier0"));
    CHECK(!strcmp(c.extra, "cpu_model=montecito unknown=1 engine=interp"));
    round_trip(&c);

    /* Invalid raw values remain visible and round-trip without substitution. */
    CHECK(parse(&c, "engine=unknown sample_sec=0 fp_host=perhaps reserve_mib= pause_sec=-2"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("engine")), "unknown"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("sample_sec")), "0"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("fp_host")), "perhaps"));
    CHECK(!c.value[index_of("reserve_mib")][0]);
    CHECK(!strcmp(cfg_value_text(&c, index_of("reserve_mib")), "Empty (invalid)"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("pause_sec")), "-2"));
    CHECK(strstr(tokens(&c), "reserve_mib= timeout_sec=0 pause_sec=-2"));
    round_trip(&c);
    CHECK(parse(&c, "reserve_mib= reserve_mib=256"));
    CHECK(!c.value[index_of("reserve_mib")][0]);
    CHECK(!strcmp(c.extra, "reserve_mib=256"));
    round_trip(&c);
    CHECK(parse(&c, "engine=0123456789012345678901234567890123456789"));
    CHECK(!strcmp(cfg_value_text(&c, index_of("engine")),
                  "0123456789012345678901234567890123456789"));
    round_trip(&c);

    /* Custom numeric values retain their text; explicit cycling chooses a preset. */
    CHECK(parse(&c, "reserve_mib=300 pause_sec=7"));
    unsigned r = index_of("reserve_mib"), p = index_of("pause_sec");
    CHECK(!strcmp(cfg_value_text(&c, r), "300"));
    CHECK(!strcmp(cfg_value_text(&c, p), "7"));
    cfg_cycle(&c, r, 1);
    CHECK(!strcmp(c.value[r], "128"));
    cfg_cycle(&c, p, -1);
    CHECK(!strcmp(c.value[p], "60"));
    round_trip(&c);

    /* Free text preserves manual overrides; file whitespace is normalized. */
    cfg_init(&c);
    for (const char *s = "helpers=off  smp_serialize=1 fp_host=0"; *s; ++s)
        CHECK(cfg_extra_put(&c, *s));
    CHECK(!cfg_extra_put(&c, '\n') && !cfg_extra_put(&c, '\t') && !cfg_extra_put(&c, (char)0x80));
    CHECK(!strcmp(c.extra, "helpers=off  smp_serialize=1 fp_host=0"));
    CHECK(strstr(tokens(&c), "helpers=off smp_serialize=1 fp_host=0"));
    cfg_extra_backspace(&c);
    CHECK(!strcmp(c.extra, "helpers=off  smp_serialize=1 fp_host="));
    for (int i = 0; i < 100; ++i)
        cfg_extra_backspace(&c);
    CHECK(!c.extra[0]);
    cfg_extra_backspace(&c);
    CHECK(!c.extra[0]);
    for (int i = 0; i < CFG_EXTRA_MAX - 1; ++i)
        CHECK(cfg_extra_put(&c, 'x'));
    CHECK(!cfg_extra_put(&c, 'x'));

    /* The token budget is exactly 511 bytes, including explicit defaults. */
    cfg_init(&c);
    {
        size_t base = strlen(tokens(&c)), padding = CFG_TOKENS_MAX - base - 1;
        CHECK(padding < CFG_EXTRA_MAX);
        memset(c.extra, 'x', padding);
        c.extra[padding] = 0;
        CHECK(strlen(tokens(&c)) == CFG_TOKENS_MAX);
        CHECK(cfg_serialize(&c, text, sizeof(text)) > 0);
        CHECK(cfg_extra_put(&c, 'x'));
        CHECK(cfg_tokens(&c, text, sizeof(text)) == CFG_TOKENS_MAX + 1);
        CHECK(cfg_serialize(&c, text, sizeof(text)) == 0);
    }
    cfg_init(&c);
    CHECK(cfg_serialize(&c, text, 40) == 0);
    CHECK(cfg_tokens(&c, text, 0) == (size_t)-1);
    {
        char big[2000] = "", extra[CFG_EXTRA_MAX];
        memset(extra, 'k', sizeof(extra) - 1);
        extra[sizeof(extra) - 1] = 0;
        strcat(big, extra);
        strcat(big, " ");
        strcat(big, extra);
        CHECK(!parse(&c, big));
        CHECK(strlen(c.extra) < CFG_EXTRA_MAX);
    }
    {
        char big[CFG_VALUE_MAX + 32];
        strcpy(big, "engine=");
        memset(big + 7, 'x', CFG_VALUE_MAX);
        big[7 + CFG_VALUE_MAX] = 0;
        CHECK(!parse(&c, big));
    }

    CHECK(cfg_page_rows(25) == 15 && cfg_page_first(CFG_SETTINGS, 15) == CFG_SETTINGS / 15 * 15);
    CHECK(cfg_page_rows(20) == 10 && cfg_page_first(CFG_SETTINGS, 10) == CFG_SETTINGS / 10 * 10);
    for (unsigned rows = 11; rows < 50; ++rows)
        for (unsigned selected = 0; selected <= CFG_SETTINGS; ++selected) {
            unsigned visible = cfg_page_rows(rows), first = cfg_page_first(selected, visible);
            CHECK(first <= selected && selected < first + visible && visible + 10 <= rows);
        }
    puts("test-vmmcfg: defaults, labels, raw values, round trips, cycling and limits passed");
    return 0;
}
