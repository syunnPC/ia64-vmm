/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_AMD64_HYPERV_KEYBOARD_H
#define VMM_AMD64_HYPERV_KEYBOARD_H
#include "ia64vmm/types.h"
typedef void (*amd64_hyperv_key_callback)(void *, uint16_t makecode, bool released, bool e0,
                                          bool e1);
/* Management CPU only. Events contain set-1 make codes and separate break
 * and prefix flags. Unicode input is ignored. Failure affects this channel. */
bool amd64_hyperv_keyboard_start(amd64_hyperv_key_callback, void *context);
void amd64_hyperv_keyboard_poll(void);
void amd64_hyperv_keyboard_stop(void);
#endif
