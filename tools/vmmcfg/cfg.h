/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMMCFG_CFG_H
#define VMMCFG_CFG_H
#include <stddef.h>

/* UEFI-independent editing logic. File format: core/options.h. */
#define CFG_SETTINGS 20
#define CFG_MAX_CHOICES 11
#define CFG_VALUE_MAX 512  /* any value within the options token budget */
#define CFG_EXTRA_MAX 256  /* the free-text line, including the NUL */
#define CFG_TOKENS_MAX 511 /* OPTIONS_FILE_MAX in core/options.h */

struct cfg_setting {
    const char *key;
    const char *label;
    const char *default_value;
    unsigned nchoices;
    const char *choices[CFG_MAX_CHOICES];
    const char *choice_labels[CFG_MAX_CHOICES];
};
extern const struct cfg_setting cfg_settings[CFG_SETTINGS];

struct cfg {
    char value[CFG_SETTINGS][CFG_VALUE_MAX]; /* raw file values, including invalid ones */
    char extra[CFG_EXTRA_MAX];               /* other tokens, blank separated */
};

/* Visible setting rows; 3 header + 6 footer + final cursor row. */
unsigned cfg_page_rows(unsigned screen_rows);
unsigned cfg_page_first(unsigned selected, unsigned visible);
void cfg_init(struct cfg *c);
/* Initialize normal user-config defaults, then apply each managed key's first
 * occurrence verbatim. Unknown keys and duplicates stay in extra, in order.
 * Returns 0 if a value or extra token cannot fit; that state must not be saved. */
int cfg_parse(struct cfg *c, const char *text, size_t len);
/* Step through concrete choices, wrapping in either direction. A custom value
 * steps to the first choice forwards or the last choice backwards. */
void cfg_cycle(struct cfg *c, unsigned i, int direction);
/* Readable preset label, original custom value, or an explicit empty warning. */
const char *cfg_value_text(const struct cfg *c, unsigned i);
/* Free-text line: printable ASCII only; returns 0 if full or not printable. */
int cfg_extra_put(struct cfg *c, char ch);
void cfg_extra_backspace(struct cfg *c);
/* The tokens joined by single spaces (settings in table order, then extra).
 * Returns the length, or (size_t)-1 if it exceeds cap - 1. */
size_t cfg_tokens(const struct cfg *c, char *out, size_t cap);
/* The file text: header comment, then one token per line. Returns its
 * length, 0 if cap is too small or the tokens exceed CFG_TOKENS_MAX. */
size_t cfg_serialize(const struct cfg *c, char *out, size_t cap);
#endif
