/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/platform.h"
#include "ia64vmm/log.h"

/* PC16550D §8.1, §§8.3–8.10. Scratch and modem-loopback probe reference:
 * Asteria's ProbeUart in
 * Source/Platform/Amd64/Firmware/DiagnosticSinkSelection.cpp.
 * TX queues exact bytes; the management CPU drains the sink. No IRQ line is
 * wired; IIR still reports pending sources for polled operation. */
static uint8_t uart_iir(struct platform_state *s)
{
    /* PC16550D §8.6 Table 4: received data (priority 2) before THRE
     * (priority 3). Reading IIR clears a THRE identification it reports;
     * writing THR or enabling ETBEI re-arms it, since the transmitter
     * here is always empty. No line-status or modem-status events occur. */
    uint8_t fifo = s->uart_fcr & 1 ? 0xc0 : 0;
    if ((s->uart_ier & 1) && s->uart_rx_ready)
        return fifo | 0x04;
    if ((s->uart_ier & 2) && s->uart_thre_pending) {
        s->uart_thre_pending = false;
        return fifo | 0x02;
    }
    return fifo | 0x01;
}
static uint8_t uart_byte_read(struct platform_state *s, uint64_t off)
{
    switch (off) {
    case 0:
        if (s->uart_lcr & 0x80)
            return s->uart_dll;
        s->uart_rx_ready = false;
        return s->uart_rx;
    case 1:
        return s->uart_lcr & 0x80 ? s->uart_dlm : s->uart_ier;
    case 2:
        return uart_iir(s);
    case 3:
        return s->uart_lcr;
    case 4:
        return s->uart_mcr;
    case 5:
        return 0x60 | s->uart_rx_ready;
    case 6:
        if (!(s->uart_mcr & 0x10))
            return 0xb0;
        return ((s->uart_mcr & 2) << 3) | ((s->uart_mcr & 1) << 5) | ((s->uart_mcr & 12) << 4);
    case 7:
        return s->uart_scr;
    default:
        return 0;
    }
}
static void uart_byte_write(struct platform_state *s, uint64_t off, uint8_t value)
{
    switch (off) {
    case 0:
        if (s->uart_lcr & 0x80)
            s->uart_dll = value;
        else {
            if (s->uart_mcr & 0x10) {
                s->uart_rx = value;
                s->uart_rx_ready = true;
            } else {
                char c = (char)value;
                vmm_guest_serial_write(&c, 1);
            }
            s->uart_thre_pending = true;
        }
        break;
    case 1:
        if (s->uart_lcr & 0x80)
            s->uart_dlm = value;
        else {
            s->uart_ier = value & 15;
            s->uart_thre_pending = (value & 2) != 0;
        }
        break;
    case 2:
        s->uart_fcr = value & 0xc9;
        if (value & 2)
            s->uart_rx_ready = false;
        break;
    case 3:
        s->uart_lcr = value;
        break;
    case 4:
        s->uart_mcr = value & 31;
        break;
    case 7:
        s->uart_scr = value;
        break;
    default:
        break;
    }
}
static uint64_t uart_read_unlocked(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    (void)v;
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i)
        value |= (uint64_t)uart_byte_read(opaque, off + i) << (i * 8);
    return value;
}
static uint64_t uart_read(void *opaque, struct vcpu *v, uint64_t off, unsigned size)
{
    platform_lock(&((struct platform_state *)opaque)->uart_lock);
    uint64_t result = uart_read_unlocked(opaque, v, off, size);
    platform_unlock(&((struct platform_state *)opaque)->uart_lock);
    return result;
}
static void uart_write_unlocked(void *opaque, struct vcpu *v, uint64_t off, uint64_t value,
                                unsigned size)
{
    (void)v;
    for (unsigned i = 0; i < size; ++i)
        uart_byte_write(opaque, off + i, value >> (i * 8));
}
static void uart_write(void *opaque, struct vcpu *v, uint64_t off, uint64_t value, unsigned size)
{
    platform_lock(&((struct platform_state *)opaque)->uart_lock);
    uart_write_unlocked(opaque, v, off, value, size);
    platform_unlock(&((struct platform_state *)opaque)->uart_lock);
}
const struct gm_mmio_ops platform_uart_ops = {uart_read, uart_write};
