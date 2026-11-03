/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_CORE_OPTIONS_H
#define IA64VMM_CORE_OPTIONS_H
#include <stddef.h>

/* The options module (the stick's \vmm-options.conf, Limine module string
 * "options"): key=value tokens, whitespace separated; blank lines and lines
 * whose first non-blank character is '#' are ignored; CRLF is whitespace.
 * The effective command line is the module's tokens, a space, then the boot
 * entry's own command line; platform_cmdline_value takes the first
 * occurrence of a key, so the module wins. */
#define OPTIONS_MODULE_NAME "options"
#define OPTIONS_MEDIA_MODULE_NAME "options-media"

/* Size of boot_info.cmdline (include/ia64vmm/bootinfo.h) and the budget the
 * stick builder and tools/vmmcfg keep to: an entry's own command line and
 * the options file's tokens are each at most 511 bytes, 511 + 1 + 511 = 1023
 * plus the terminator. */
#define OPTIONS_CMDLINE_SIZE 1024
#define OPTIONS_ENTRY_MAX 511
#define OPTIONS_FILE_MAX 511

enum options_status {
    OPTIONS_OK,
    OPTIONS_OVERFLOW, /* the merged line does not fit */
    OPTIONS_BAD_BYTE, /* a control or non-ASCII byte outside whitespace */
};

/* Merge the options module text [file, file+file_len) with the entry's
 * command line (NULL = empty) into out[cap]. A token of the entry whose key
 * (text before '=', else the whole token) equals the key of a module token
 * is dropped, so that single-valued keys (vcpus, placement, pause_sec: the
 * boot code rejects duplicates, reserve_mib takes the last) have one value;
 * "selftest" is a repeatable flag and is never dropped. With no module
 * tokens the entry's line is copied unchanged. *file_tokens receives the
 * number of tokens taken from the module. On failure out holds "". */
enum options_status options_merge(const char *file, size_t file_len, const char *entry, char *out,
                                  size_t cap, unsigned *file_tokens);

/* Merge the first vmm_media= and guest_media= tokens from the module. Unrelated
 * tokens and bytes are ignored; absent media leaves the entry unchanged. */
enum options_status options_merge_media(const char *file, size_t file_len, const char *entry,
                                        char *out, size_t cap, unsigned *file_tokens);
/* Hyper-V path 0, target 0, explicitly selected LUN 0..63; -1 if invalid. */
int options_hyperv_lun(const char *, size_t);

/* Smallest reserve_mib Tier1 (and the host-window selftests) run with:
 * 512 MiB plus 8 MiB per vCPU beyond the first. */
unsigned long long options_tier1_reserve_floor_mib(unsigned vcpus);

/* Length of the next piece of s (at most width bytes, cut at a space where
 * possible) for logging a long line in several short ones. */
size_t options_chunk(const char *s, size_t width);
#endif
