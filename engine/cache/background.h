/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef IA64VMM_CACHE_BACKGROUND_H
#define IA64VMM_CACHE_BACKGROUND_H
/* Stopped-executor flushes bracket background readers before code_lock.
 * Nesting is management-only; NULL hooks preserve standalone fixtures. */
void code_background_set(void (*quiesce)(void), void (*resume)(void));
void code_background_quiesce(void);
void code_background_resume(void);
#endif
