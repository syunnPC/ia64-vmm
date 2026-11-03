/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_LOG_INTERNAL_H
#define IA64VMM_LOG_INTERNAL_H
#include "ia64vmm/log.h"

#define LOG_RING_SIZE (1024u * 1024u)
#define LOG_RING_MAGIC UINT64_C(0x494136344c4f4752)
/* Positions are monotonic byte counts; retained bytes begin at
 * max(0, write_pos - LOG_RING_SIZE). flushed_pos tracks media persistence,
 * not mirroring to debug/FB sinks. */
struct log_ring {
    uint64_t magic, generation, write_pos, flushed_pos;
    char data[LOG_RING_SIZE];
};
const struct log_ring *log_get_ring(void);
void log_set_fb_sink(void (*sink)(const char *s, size_t n));
/* Verbosity filters debug/FB mirrors; RAM always retains every level. */
void log_set_level(enum log_level level);
void log_flush_guest_text(void);
/* Boot context before APs: move all log storage to owned pages, then route
 * mirrors through the management CPU. Drain only from management context. */
void log_relocate(void);
void log_set_async(void);
void log_drain(void);
/* Panic context: ring-lock bypass when the interrupted owner cannot run. */
void log_panic_begin(void);
/* Final-summary support: copy the newest `max` complete ring
 * lines that begin with `prefix`, oldest first, each cut to
 * LOG_MATCH_COLUMNS characters. Returns the number of matching lines in the
 * retained ring; *kept receives how many were copied (at most max). */
#define LOG_MATCH_COLUMNS 168u
/* The last `max` complete lines written at or after ring position `from` (a
 * write_pos taken earlier), oldest first, each cut to LOG_MATCH_COLUMNS.
 * Returns how many were copied; *total counts every such line. */
size_t log_ring_since(const struct log_ring *r, uint64_t from, char (*lines)[LOG_MATCH_COLUMNS + 1],
                      size_t max, size_t *total);
size_t log_ring_matching(const struct log_ring *r, const char *prefix,
                         char (*lines)[LOG_MATCH_COLUMNS + 1], size_t max, size_t *kept);
/* PLOG management snapshots are locked; cursor advances only in the copy.
 * Fixed storage is reserved before allocator initialization for warm reset. */
void log_relocate_fixed(void *storage);
bool log_in_panic(void);
uint64_t log_media_end(void);
size_t log_media_read(uint64_t *cursor, void *data, size_t size, uint64_t *lost);
void log_media_flushed(uint64_t position);
size_t log_crash_read(uint64_t *cursor, void *data, size_t size);
void log_media_clean(void);
#endif
