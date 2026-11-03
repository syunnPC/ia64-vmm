/* SPDX-License-Identifier: GPL-2.0-or-later */
/* IBM PS/2 keyboard/controller commands and scan code sets 1 and 2. */
#include "platform/i8042.h"
#include "platform/lock.h"
#include "ia64vmm/string.h"

#define QUEUE_SIZE 256u
#define RESPONSE_RESERVE 16u
#define PENDING_KEYS 32u
enum controller_parameter {
    CTL_NONE,
    CTL_CONFIG,
    CTL_OUTPUT,
    CTL_KBD_OUTPUT,
    CTL_AUX_OUTPUT,
    CTL_AUX_COMMAND
};
enum keyboard_parameter { KBD_NONE, KBD_LEDS, KBD_SCAN_SET, KBD_TYPEMATIC };
struct key_event {
    uint16_t makecode;
    bool released, e0, e1;
};
static bool configured;
static unsigned controller_lock;
static struct {
    void (*irq)(void *, bool);
    void *context;
    uint8_t queue[QUEUE_SIZE];
    bool keyboard[QUEUE_SIZE];
    unsigned head, count;
    struct key_event pending[PENDING_KEYS];
    unsigned pending_head, pending_count;
    uint8_t config, output, scan_set, leds, typematic, last;
    enum controller_parameter controller_parameter;
    enum keyboard_parameter keyboard_parameter;
    bool active, scanning, level, last_command;
} controller;
static void drain_input(void);

/* Numeric scan codes from the IBM keyboard scan-set tables. */
static const uint8_t set2_make[0x59] = {
    [0x01] = 0x76, [0x02] = 0x16, [0x03] = 0x1e, [0x04] = 0x26, [0x05] = 0x25, [0x06] = 0x2e,
    [0x07] = 0x36, [0x08] = 0x3d, [0x09] = 0x3e, [0x0a] = 0x46, [0x0b] = 0x45, [0x0c] = 0x4e,
    [0x0d] = 0x55, [0x0e] = 0x66, [0x0f] = 0x0d, [0x10] = 0x15, [0x11] = 0x1d, [0x12] = 0x24,
    [0x13] = 0x2d, [0x14] = 0x2c, [0x15] = 0x35, [0x16] = 0x3c, [0x17] = 0x43, [0x18] = 0x44,
    [0x19] = 0x4d, [0x1a] = 0x54, [0x1b] = 0x5b, [0x1c] = 0x5a, [0x1d] = 0x14, [0x1e] = 0x1c,
    [0x1f] = 0x1b, [0x20] = 0x23, [0x21] = 0x2b, [0x22] = 0x34, [0x23] = 0x33, [0x24] = 0x3b,
    [0x25] = 0x42, [0x26] = 0x4b, [0x27] = 0x4c, [0x28] = 0x52, [0x29] = 0x0e, [0x2a] = 0x12,
    [0x2b] = 0x5d, [0x2c] = 0x1a, [0x2d] = 0x22, [0x2e] = 0x21, [0x2f] = 0x2a, [0x30] = 0x32,
    [0x31] = 0x31, [0x32] = 0x3a, [0x33] = 0x41, [0x34] = 0x49, [0x35] = 0x4a, [0x36] = 0x59,
    [0x37] = 0x7c, [0x38] = 0x11, [0x39] = 0x29, [0x3a] = 0x58, [0x3b] = 0x05, [0x3c] = 0x06,
    [0x3d] = 0x04, [0x3e] = 0x0c, [0x3f] = 0x03, [0x40] = 0x0b, [0x41] = 0x83, [0x42] = 0x0a,
    [0x43] = 0x01, [0x44] = 0x09, [0x45] = 0x77, [0x46] = 0x7e, [0x47] = 0x6c, [0x48] = 0x75,
    [0x49] = 0x7d, [0x4a] = 0x7b, [0x4b] = 0x6b, [0x4c] = 0x73, [0x4d] = 0x74, [0x4e] = 0x79,
    [0x4f] = 0x69, [0x50] = 0x72, [0x51] = 0x7a, [0x52] = 0x70, [0x53] = 0x71, [0x54] = 0x84,
    [0x56] = 0x61, [0x57] = 0x78, [0x58] = 0x07,
};
static uint8_t translate(uint8_t byte)
{
    switch (byte) {
    case 0x02:
        return 0x41;
    case 0x1f:
        return 0x5b;
    case 0x27:
        return 0x5c;
    case 0x2f:
        return 0x5d;
    case 0x37:
        return 0x5e;
    case 0x3f:
        return 0x5f;
    case 0x5e:
        return 0x63;
    default:
        break;
    }
    for (unsigned i = 1; i < IA64VMM_ARRAY_SIZE(set2_make); ++i)
        if (set2_make[i] == byte)
            return i;
    return byte;
}
static uint8_t set2_code(unsigned code, bool extended)
{
    if (extended)
        switch (code) {
        case 0x5b:
            return 0x1f;
        case 0x5c:
            return 0x27;
        case 0x5d:
            return 0x2f;
        case 0x5e:
            return 0x37;
        case 0x5f:
            return 0x3f;
        case 0x63:
            return 0x5e;
        default:
            break;
        }
    return code < IA64VMM_ARRAY_SIZE(set2_make) ? set2_make[code] : 0;
}
static void set_irq(bool high)
{
    if (high == controller.level)
        return;
    controller.level = high;
    if (controller.irq)
        controller.irq(controller.context, high);
}
static void irq_update(void)
{
    set_irq(controller.active && controller.count && (controller.config & 1));
}
static bool enqueue(const uint8_t *bytes, unsigned length, bool keyboard, bool input,
                    bool translated)
{
    uint8_t output[16];
    unsigned count = 0;
    bool release = false;
    for (unsigned i = 0; i < length; ++i) {
        uint8_t byte = bytes[i];
        if (translated && (controller.config & 0x40)) {
            if (byte == 0xf0) {
                release = true;
                continue;
            }
            if (byte != 0xe0 && byte != 0xe1) {
                byte = translate(byte) | (release ? 0x80 : 0);
                release = false;
            }
        }
        if (count == sizeof(output))
            return false;
        output[count++] = byte;
    }
    if (count + (input ? RESPONSE_RESERVE : 0) > QUEUE_SIZE - controller.count)
        return false;
    for (unsigned i = 0; i < count; ++i) {
        unsigned at = (controller.head + controller.count++) & (QUEUE_SIZE - 1);
        controller.queue[at] = output[i];
        controller.keyboard[at] = keyboard;
    }
    irq_update();
    return true;
}
static void reply(uint8_t byte, bool keyboard)
{
    enqueue(&byte, 1, keyboard, false, keyboard);
}
static void keyboard_defaults(void)
{
    controller.scan_set = 2;
    controller.leds = 0;
    controller.typematic = 0x2b;
    controller.scanning = true;
    controller.keyboard_parameter = KBD_NONE;
}
static void reset(void)
{
    set_irq(false);
    controller.head = controller.count = 0;
    controller.config = 0x70;
    controller.output = 3;
    controller.pending_head = controller.pending_count = 0;
    controller.controller_parameter = CTL_NONE;
    controller.last_command = false;
    controller.last = 0xfe;
    keyboard_defaults();
}
void platform_i8042_configure(bool enabled)
{
    platform_lock(&controller_lock);
    configured = enabled;
    if (!enabled) {
        reset();
        controller.active = false;
    }
    platform_unlock(&controller_lock);
}
bool platform_i8042_configured(void)
{
    return configured;
}
bool platform_i8042_init(void (*irq)(void *, bool), void *context)
{
    platform_lock(&controller_lock);
    if (!configured || controller.active) {
        platform_unlock(&controller_lock);
        return false;
    }
    controller.irq = irq;
    controller.context = context;
    controller.active = true;
    reset();
    platform_unlock(&controller_lock);
    return true;
}
void platform_i8042_reset(void)
{
    platform_lock(&controller_lock);
    reset();
    platform_unlock(&controller_lock);
}
void platform_i8042_cleanup(void)
{
    platform_lock(&controller_lock);
    set_irq(false);
    memset(&controller, 0, sizeof(controller));
    platform_unlock(&controller_lock);
}
uint8_t platform_i8042_read(uint16_t port)
{
    platform_lock(&controller_lock);
    uint8_t value = 0xff;
    if (controller.active && port == 0x64)
        value = (controller.config & 4) | ((controller.config & 0x10) ? 0 : 0x10) |
                (controller.last_command ? 8 : 0) | (controller.count ? 1 : 0);
    if (controller.active && port == 0x60 && controller.count) {
        set_irq(false);
        unsigned at = controller.head;
        value = controller.queue[at];
        if (controller.keyboard[at] && value != 0xfe)
            controller.last = value;
        controller.head = (at + 1) & (QUEUE_SIZE - 1);
        --controller.count;
        drain_input();
        irq_update();
    }
    platform_unlock(&controller_lock);
    return value;
}
static void controller_command(uint8_t value)
{
    controller.controller_parameter = CTL_NONE;
    switch (value) {
    case 0x20:
        reply(controller.config, false);
        break;
    case 0x60:
        controller.controller_parameter = CTL_CONFIG;
        break;
    case 0xa7:
    case 0xa8:
        controller.config |= 0x20;
        break;
    case 0xa9:
        reply(1, false);
        break;
    case 0xaa:
        controller.config |= 4;
        reply(0x55, false);
        break;
    case 0xab:
        reply(0, false);
        break;
    case 0xad:
        controller.config |= 0x10;
        break;
    case 0xae:
        controller.config &= ~0x10;
        break;
    case 0xc0:
        reply(0x80, false);
        break;
    case 0xd0:
        reply(controller.output | (controller.count ? 0x10 : 0), false);
        break;
    case 0xd1:
        controller.controller_parameter = CTL_OUTPUT;
        break;
    case 0xd2:
        controller.controller_parameter = CTL_KBD_OUTPUT;
        break;
    case 0xd3:
        controller.controller_parameter = CTL_AUX_OUTPUT;
        break;
    case 0xd4:
        controller.controller_parameter = CTL_AUX_COMMAND;
        break;
    case 0xe0:
        reply(3, false);
        break;
    default:
        break;
    }
    irq_update();
}
static bool keyboard_command_byte(uint8_t value)
{
    return value == 0xed || value == 0xee || value == 0xf0 || value == 0xf2 || value == 0xf3 ||
           value == 0xf4 || value == 0xf5 || value == 0xf6 || value == 0xfe || value == 0xff;
}
static void keyboard_command(uint8_t value)
{
    if (keyboard_command_byte(value))
        controller.keyboard_parameter = KBD_NONE;
    if (controller.keyboard_parameter != KBD_NONE) {
        switch (controller.keyboard_parameter) {
        case KBD_LEDS:
            if (value > 7) {
                reply(0xfe, true);
                return;
            }
            controller.leds = value;
            break;
        case KBD_SCAN_SET:
            if (value > 2) {
                reply(0xfe, true);
                return;
            }
            reply(0xfa, true);
            if (value)
                controller.scan_set = value;
            else
                reply(controller.scan_set, true);
            controller.keyboard_parameter = KBD_NONE;
            return;
        case KBD_TYPEMATIC:
            if (value & 0x80) {
                reply(0xfe, true);
                return;
            }
            controller.typematic = value;
            break;
        default:
            break;
        }
        controller.keyboard_parameter = KBD_NONE;
        reply(0xfa, true);
        return;
    }
    switch (value) {
    case 0xed:
        controller.keyboard_parameter = KBD_LEDS;
        reply(0xfa, true);
        break;
    case 0xee:
        reply(0xee, true);
        break;
    case 0xf0:
        controller.keyboard_parameter = KBD_SCAN_SET;
        reply(0xfa, true);
        break;
    case 0xf2:
        enqueue((const uint8_t[]){0xfa, 0xab, 0x83}, 3, true, false, true);
        break;
    case 0xf3:
        controller.keyboard_parameter = KBD_TYPEMATIC;
        reply(0xfa, true);
        break;
    case 0xf4:
        controller.scanning = true;
        reply(0xfa, true);
        break;
    case 0xf5:
        keyboard_defaults();
        controller.scanning = false;
        reply(0xfa, true);
        break;
    case 0xf6:
        keyboard_defaults();
        reply(0xfa, true);
        break;
    case 0xfe:
        enqueue(&controller.last, 1, true, false, false);
        break;
    case 0xff:
        controller.head = controller.count = 0;
        keyboard_defaults();
        controller.pending_head = controller.pending_count = 0;
        enqueue((const uint8_t[]){0xfa, 0xaa}, 2, true, false, true);
        break;
    default:
        reply(0xfe, true);
        break;
    }
}
void platform_i8042_write(uint16_t port, uint8_t value)
{
    platform_lock(&controller_lock);
    if (controller.active && (port == 0x60 || port == 0x64)) {
        controller.last_command = port == 0x64;
        if (port == 0x64)
            controller_command(value);
        else {
            enum controller_parameter pending = controller.controller_parameter;
            controller.controller_parameter = CTL_NONE;
            switch (pending) {
            case CTL_CONFIG:
                controller.config = value | 0x20;
                irq_update();
                break;
            case CTL_OUTPUT:
                controller.output = value | 1;
                break;
            case CTL_KBD_OUTPUT:
                reply(value, false);
                break;
            case CTL_AUX_OUTPUT:
            case CTL_AUX_COMMAND:
                break;
            default:
                keyboard_command(value);
                break;
            }
        }
    }
    drain_input();
    platform_unlock(&controller_lock);
}
static bool input(uint16_t makecode, bool released, bool e0, bool e1, bool defer)
{
    if (!controller.active || !controller.scanning || (controller.config & 0x10) || !makecode ||
        makecode > 0x7f || (e0 && e1) ||
        (e1 ? makecode != 0x45 && makecode != 0x1d : !set2_code(makecode, e0)))
        return false;
    if (defer && (controller.keyboard_parameter != KBD_NONE || controller.pending_count)) {
        if (controller.pending_count == PENDING_KEYS)
            return false;
        unsigned at = (controller.pending_head + controller.pending_count++) % PENDING_KEYS;
        controller.pending[at] = (struct key_event){makecode, released, e0, e1};
        return true;
    }
    uint8_t bytes[10];
    unsigned length = 0;
    if (e1) {
        if (!released) {
            if (controller.scan_set == 1) {
                const uint8_t pause[] = {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5};
                memcpy(bytes, pause, sizeof(pause));
                length = sizeof(pause);
            } else {
                const uint8_t pause[] = {0xe1, 0x14, 0x77, 0xe1, 0xf0, 0x14, 0xf0, 0x77};
                memcpy(bytes, pause, sizeof(pause));
                length = sizeof(pause);
            }
        }
    } else if (e0 && makecode == 0x37) {
        if (controller.scan_set == 1) {
            bytes[0] = 0xe0;
            bytes[1] = released ? 0xb7 : 0x2a;
            bytes[2] = 0xe0;
            bytes[3] = released ? 0xaa : 0x37;
            length = 4;
        } else if (released) {
            const uint8_t print[] = {0xe0, 0xf0, 0x7c, 0xe0, 0xf0, 0x12};
            memcpy(bytes, print, sizeof(print));
            length = sizeof(print);
        } else {
            const uint8_t print[] = {0xe0, 0x12, 0xe0, 0x7c};
            memcpy(bytes, print, sizeof(print));
            length = sizeof(print);
        }
    } else {
        uint8_t code = set2_code(makecode, e0);
        if (e0)
            bytes[length++] = 0xe0;
        if (controller.scan_set == 2) {
            if (released)
                bytes[length++] = 0xf0;
            bytes[length++] = code;
        } else
            bytes[length++] = makecode | (released ? 0x80 : 0);
    }
    return enqueue(bytes, length, true, true, true);
}
static void drain_input(void)
{
    if (!controller.scanning)
        controller.pending_head = controller.pending_count = 0;
    while (controller.pending_count && controller.keyboard_parameter == KBD_NONE) {
        const struct key_event *key = controller.pending + controller.pending_head;
        if (!input(key->makecode, key->released, key->e0, key->e1, false))
            break;
        controller.pending_head = (controller.pending_head + 1) % PENDING_KEYS;
        --controller.pending_count;
    }
}
bool platform_i8042_input(uint16_t makecode, bool released, bool e0, bool e1)
{
    platform_lock(&controller_lock);
    bool accepted = input(makecode, released, e0, e1, true);
    platform_unlock(&controller_lock);
    return accepted;
}
