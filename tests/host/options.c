/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "core/options.h"

static const char *merge(const char *file, const char *entry, unsigned *tokens,
                         enum options_status *status)
{
    static char out[OPTIONS_CMDLINE_SIZE];
    *status = options_merge(file, file ? strlen(file) : 0, entry, out, sizeof(out), tokens);
    return out;
}

static void test_options_media(void)
{
    char out[OPTIONS_CMDLINE_SIZE];
    unsigned n;
    const char *media_options =
        "guest_media=hyperv:1 engine=tier1 vmm_media=hyperv:0 guest_media=hyperv:2";
    CHECK(options_merge_media(media_options, strlen(media_options), "vmm_media=list engine=interp",
                              out, sizeof(out), &n) == OPTIONS_OK);
    CHECK(n == 2 && !strcmp(out, "guest_media=hyperv:1 vmm_media=hyperv:0 engine=interp"));
    CHECK(options_hyperv_lun("hyperv:0", 8) == 0);
    CHECK(options_hyperv_lun("hyperv:63", 9) == 63);
    const char *invalid[] = {"hyperv:", "hyperv:64", "hyperv:-1", "hyperv:1x", "00:00.0"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        CHECK(options_hyperv_lun(invalid[i], strlen(invalid[i])) == -1);
    const char *file = "engine=tier1 reserve_mib=1 vcpus=9 selftest=pf vmm_media=00:14.0";
    const char *entry = "engine=tier0 reserve_mib=512 vcpus=1 vmm_media=01:00.0 selftest=timer";
    CHECK(options_merge_media(file, strlen(file), entry, out, sizeof(out), &n) == OPTIONS_OK);
    CHECK(n == 1 &&
          !strcmp(out, "vmm_media=00:14.0 engine=tier0 reserve_mib=512 vcpus=1 selftest=timer"));
    file = "fp_experimental=1 fp_approx=1 fp_host=1 vmm_media=00:14.0";
    CHECK(options_merge(file, strlen(file), entry, out, sizeof(out), &n) == OPTIONS_OK);
    CHECK(strstr(out, "fp_experimental=1") && strstr(out, "fp_approx=1"));
    CHECK(options_merge_media(file, strlen(file), entry, out, sizeof(out), &n) == OPTIONS_OK);
    CHECK(n == 1 &&
          !strcmp(out, "vmm_media=00:14.0 engine=tier0 reserve_mib=512 vcpus=1 selftest=timer"));
    file = "# vmm_media=ff:00.0\r\n vmm_media_extra=ff:00.0 vmm_media vmm_mediax=ff:00.0\n"
           "vmm_media=00:14.0 vmm_media=01:00.0";
    CHECK(options_merge_media(file, strlen(file), "exit=halt", out, sizeof(out), &n) == OPTIONS_OK);
    CHECK(n == 1 && !strcmp(out, "vmm_media=00:14.0 exit=halt"));
    file = "engine=\xc3\xa9\x01\n# \xff\nvmm_media=00:14.0\nignored=\x7f";
    CHECK(options_merge_media(file, strlen(file), NULL, out, sizeof(out), &n) == OPTIONS_OK);
    CHECK(n == 1 && !strcmp(out, "vmm_media=00:14.0"));
    file = "# vmm_media=00:14.0\nengine=\xff vmm_media_extra=00:14.0";
    CHECK(options_merge_media(file, strlen(file), entry, out, sizeof(out), &n) == OPTIONS_OK);
    CHECK(n == 0 && !strcmp(out, entry));
    CHECK(options_merge_media(NULL, 0, "exit=halt  vcpus=1", out, sizeof(out), &n) == OPTIONS_OK);
    CHECK(n == 0 && !strcmp(out, "exit=halt  vcpus=1"));
    file = "vmm_media=00:14.0\x01 vmm_media=01:00.0";
    CHECK(options_merge_media(file, strlen(file), entry, out, sizeof(out), &n) ==
              OPTIONS_BAD_BYTE &&
          !out[0]);
    file = "vmm_media=\xc3\xa9";
    CHECK(options_merge_media(file, strlen(file), entry, out, sizeof(out), &n) ==
              OPTIONS_BAD_BYTE &&
          !out[0]);
    file = "vmm_media=00:14.0\0bad";
    CHECK(options_merge_media(file, sizeof("vmm_media=00:14.0\0bad") - 1, entry, out, sizeof(out),
                              &n) == OPTIONS_BAD_BYTE &&
          !out[0]);
    file = "vmm_media=00:14.0 vmm_media=\xff";
    CHECK(options_merge_media(file, strlen(file), "", out, 18, &n) == OPTIONS_OK);
    CHECK(n == 1 && !strcmp(out, "vmm_media=00:14.0"));
    CHECK(options_merge_media(file, strlen(file), "", out, 17, &n) == OPTIONS_OVERFLOW && !out[0]);
    CHECK(options_merge_media(file, strlen(file), "x=1", out, 20, &n) == OPTIONS_OVERFLOW &&
          !out[0]);
    CHECK(options_merge_media("", 0, entry, out, 8, &n) == OPTIONS_OVERFLOW && !out[0]);
    CHECK(options_merge_media(file, strlen(file), "", out, 0, &n) == OPTIONS_OVERFLOW);
}

void test_options(void)
{
    test_options_media();
    unsigned n;
    enum options_status st;
    /* Module tokens first, then the entry; same key: the module wins. */
    CHECK(!strcmp(merge("engine=interp\n", "engine=tier0 reserve_mib=128", &n, &st),
                  "engine=interp reserve_mib=128"));
    CHECK(st == OPTIONS_OK && n == 1);
    CHECK(!strcmp(merge("cpu_model=madison\n", "cpu_model=montecito engine=tier0", &n, &st),
                  "cpu_model=madison engine=tier0"));
    CHECK(st == OPTIONS_OK && n == 1);
    CHECK(!strcmp(merge("cpu_model=madison cpu_model=montecito", "cpu_model=bad", &n, &st),
                  "cpu_model=madison cpu_model=montecito"));
    /* Comments, blank lines, CRLF, several tokens per line, tabs. */
    CHECK(!strcmp(
        merge(
            "# header\r\n\r\n  # indented comment\r\nengine=interp pause_sec=5\r\n\tbootmenu=1 \r\n# end",
            "engine=tier0 exit=halt pause_sec=20 placement=physical-core", &n, &st),
        "engine=interp pause_sec=5 bootmenu=1 exit=halt placement=physical-core"));
    CHECK(st == OPTIONS_OK && n == 3);
    /* '#' inside a token is data, not a comment. */
    CHECK(!strcmp(merge("a=b#c\n", "x=1", &n, &st), "a=b#c x=1") && n == 1);
    /* Empty or comment-only module, absent module: the entry is unchanged
     * (even its spacing), no tokens. */
    CHECK(!strcmp(merge("", "exit=halt  reserve_mib=128", &n, &st), "exit=halt  reserve_mib=128") &&
          n == 0 && st == OPTIONS_OK);
    CHECK(!strcmp(merge("# only comments\n\n", "exit=halt", &n, &st), "exit=halt") && n == 0);
    CHECK(!strcmp(merge(NULL, "exit=halt", &n, &st), "exit=halt") && n == 0);
    CHECK(!strcmp(merge("engine=interp", NULL, &n, &st), "engine=interp") && n == 1);
    /* Flag tokens (no '='), keys that only share a prefix, repeatable selftest. */
    CHECK(!strcmp(merge("flag engine=tier1 selftest=host\n",
                        "flag engine_x=1 selftest=timer selftest=host engine=interp", &n, &st),
                  "flag engine=tier1 selftest=host engine_x=1 selftest=timer selftest=host"));
    /* Keys the boot code rejects twice are never duplicated. */
    CHECK(!strcmp(merge("placement=logical-thread vcpus=2 reserve_mib=256",
                        "reserve_mib=512 vcpus=1 placement=physical-core pause_sec=0", &n, &st),
                  "placement=logical-thread vcpus=2 reserve_mib=256 pause_sec=0"));
    /* Tier1 reserve floor: 512 MiB plus 8 MiB per vCPU beyond the first. */
    CHECK(options_tier1_reserve_floor_mib(1) == 512 && options_tier1_reserve_floor_mib(2) == 520 &&
          options_tier1_reserve_floor_mib(4) == 536 && options_tier1_reserve_floor_mib(32) == 760);
    /* Control and non-ASCII bytes are refused. */
    merge("engine=interp\x01\n", "x=1", &n, &st);
    CHECK(st == OPTIONS_BAD_BYTE);
    merge("a=\xc3\xa9\n", "x=1", &n, &st);
    CHECK(st == OPTIONS_BAD_BYTE);
    { /* A real NUL inside the module is a bad byte. */
        char out[64];
        unsigned t;
        CHECK(options_merge("a=b\0c", 5, "x=1", out, sizeof(out), &t) == OPTIONS_BAD_BYTE &&
              !out[0]);
    }
    /* Overflow: exact fit passes, one byte more is refused, out is empty. */
    {
        char file[700], entry[600], out[OPTIONS_CMDLINE_SIZE], small[16];
        unsigned t;
        memset(file, 'f', 511);
        file[511] = 0;
        memset(entry, 'e', 511);
        entry[511] = 0;
        CHECK(options_merge(file, strlen(file), entry, out, sizeof(out), &t) == OPTIONS_OK);
        CHECK(strlen(out) == 1023 && t == 1);
        memset(entry, 'e', 512);
        entry[512] = 0;
        CHECK(options_merge(file, strlen(file), entry, out, sizeof(out), &t) == OPTIONS_OVERFLOW &&
              !out[0]);
        memset(file, 'f', 600);
        file[600] = 0; /* module alone too large */
        CHECK(options_merge(file, strlen(file), "x=1", small, sizeof(small), &t) ==
                  OPTIONS_OVERFLOW &&
              !small[0]);
        CHECK(options_merge("", 0, "0123456789abcde", small, sizeof(small), &t) ==
              OPTIONS_OK); /* 15 + NUL */
        CHECK(options_merge("", 0, "0123456789abcdef", small, sizeof(small), &t) ==
              OPTIONS_OVERFLOW);
        CHECK(options_merge("a=1", 3, "b=2", small, 0, &t) == OPTIONS_OVERFLOW);
    }
    /* Log chunks: at most the width, cut at a space, the pieces rebuild the line. */
    {
        const char *line =
            "engine=interp pause_sec=5 bootmenu=1 exit=halt placement=physical-core selftest=host";
        char rebuilt[256] = "";
        for (const char *s = line; *s;) {
            size_t c = options_chunk(s, 30);
            CHECK(c > 0 && c <= 30);
            strncat(rebuilt, s, c);
            s += c;
            while (*s == ' ')
                ++s;
            if (*s)
                strcat(rebuilt, " ");
        }
        CHECK(!strcmp(rebuilt, line));
        CHECK(options_chunk("short", 30) == 5);
        CHECK(options_chunk("0123456789012345678901234567890123456789", 30) == 30);
    }
}
