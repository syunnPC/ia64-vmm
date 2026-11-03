/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/options.h"
#include "ia64vmm/string.h"

static int blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Next token of a stretch of text: skips blanks (and, for the module,
 * '#' comment lines). Returns the token start or NULL; *len is its length. */
static const char *next_token(const char **at, const char *end, int comments, size_t *len)
{
    const char *p = *at;
    for (;;) {
        while (p < end && blank(*p))
            ++p;
        if (p < end && comments && *p == '#') {
            while (p < end && *p != '\n')
                ++p;
            continue;
        }
        break;
    }
    if (p >= end) {
        *at = p;
        return NULL;
    }
    const char *start = p;
    while (p < end && !blank(*p))
        ++p;
    *len = (size_t)(p - start);
    *at = p;
    return start;
}
static size_t key_length(const char *t, size_t len)
{
    size_t n = 0;
    while (n < len && t[n] != '=')
        ++n;
    return n;
}
static int is_repeatable(const char *t, size_t key)
{
    return key == 8 && !strncmp(t, "selftest", 8);
}

static enum options_status merge(const char *file, size_t file_len, const char *entry, char *out,
                                 size_t cap, unsigned *file_tokens)
{
    size_t used = 0;
    unsigned count = 0;
    if (!file) {
        file = "";
        file_len = 0;
    }
    const char *end = file + file_len, *at, *tok;
    size_t len;
    if (file_tokens)
        *file_tokens = 0;
    if (!cap)
        return OPTIONS_OVERFLOW;
    out[0] = 0;
    if (!entry)
        entry = "";
    for (const char *p = file; p < end; ++p)
        if (!blank(*p) && ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e))
            return OPTIONS_BAD_BYTE;
    for (at = file; (tok = next_token(&at, end, 1, &len)); ++count) {
        if (used + (used != 0) + len + 1 > cap) {
            out[0] = 0;
            return OPTIONS_OVERFLOW;
        }
        if (used)
            out[used++] = ' ';
        memcpy(out + used, tok, len);
        used += len;
    }
    if (file_tokens)
        *file_tokens = count;
    size_t entry_len = strlen(entry);
    if (!count) {
        if (entry_len + 1 > cap)
            return OPTIONS_OVERFLOW;
        memcpy(out, entry, entry_len + 1);
        return OPTIONS_OK;
    }
    const char *entry_end = entry + entry_len;
    for (at = entry; (tok = next_token(&at, entry_end, 0, &len));) {
        size_t key = key_length(tok, len);
        int shadowed = 0;
        const char *f = file;
        size_t flen;
        const char *ftok;
        if (!is_repeatable(tok, key))
            while ((ftok = next_token(&f, end, 1, &flen)))
                if (key_length(ftok, flen) == key && !strncmp(ftok, tok, key)) {
                    shadowed = 1;
                    break;
                }
        if (shadowed)
            continue;
        if (used + 1 + len + 1 > cap) {
            out[0] = 0;
            return OPTIONS_OVERFLOW;
        }
        out[used++] = ' ';
        memcpy(out + used, tok, len);
        used += len;
    }
    out[used] = 0;
    return OPTIONS_OK;
}

enum options_status options_merge(const char *file, size_t file_len, const char *entry, char *out,
                                  size_t cap, unsigned *file_tokens)
{
    return merge(file, file_len, entry, out, cap, file_tokens);
}

enum options_status options_merge_media(const char *file, size_t file_len, const char *entry,
                                        char *out, size_t cap, unsigned *file_tokens)
{
    if (!file) {
        file = "";
        file_len = 0;
    }
    const char *at = file, *end = file + file_len, *tok;
    size_t len, used = 0;
    char selected[OPTIONS_FILE_MAX + 1];
    unsigned seen = 0;
    while ((tok = next_token(&at, end, 1, &len))) {
        unsigned kind = len >= 10 && !strncmp(tok, "vmm_media=", 10)     ? 1
                        : len >= 12 && !strncmp(tok, "guest_media=", 12) ? 2
                                                                         : 0;
        if (!kind || (seen & kind))
            continue;
        if (len >= sizeof(selected) - used - (used != 0)) {
            if (cap)
                out[0] = 0;
            if (file_tokens)
                *file_tokens = 0;
            return OPTIONS_OVERFLOW;
        }
        if (used)
            selected[used++] = ' ';
        memcpy(selected + used, tok, len);
        used += len;
        seen |= kind;
    }
    return merge(selected, used, entry, out, cap, file_tokens);
}

int options_hyperv_lun(const char *value, size_t len)
{
    if (!value || len < 8 || len > 9 || strncmp(value, "hyperv:", 7) != 0)
        return -1;
    unsigned lun = 0;
    for (size_t i = 7; i < len; ++i) {
        if (value[i] < '0' || value[i] > '9')
            return -1;
        lun = lun * 10 + (unsigned)(value[i] - '0');
    }
    return lun < 64 ? (int)lun : -1;
}

size_t options_chunk(const char *s, size_t width)
{
    size_t len = strlen(s);
    if (len <= width)
        return len;
    size_t cut = width;
    while (cut && s[cut] != ' ')
        --cut;
    return cut ? cut : width;
}

unsigned long long options_tier1_reserve_floor_mib(unsigned vcpus)
{
    return 512ull + 8ull * (vcpus ? vcpus - 1 : 0);
}
