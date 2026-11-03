/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/log_internal.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/string.h"

struct output {
    char *buf;
    size_t size, count;
};

static void repeat(struct output *o, char c, size_t n)
{
    size_t room = o->count < o->size ? o->size - 1 - o->count : 0;
    size_t take = n < room ? n : room;
    if (take)
        memset(o->buf + o->count, c, take);
    o->count += n;
}

static void append(struct output *o, const char *s, size_t n)
{
    size_t room = o->count < o->size ? o->size - 1 - o->count : 0;
    size_t take = n < room ? n : room;
    if (take)
        memcpy(o->buf + o->count, s, take);
    o->count += n;
}

int vmm_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct output o = {buf, size, 0};
    while (*fmt) {
        if (*fmt != '%') {
            append(&o, fmt++, 1);
            continue;
        }
        ++fmt;
        bool left = false, zero = false;
        while (*fmt == '-' || *fmt == '0') {
            left |= *fmt == '-';
            zero |= *fmt == '0';
            ++fmt;
        }
        size_t width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            unsigned d = (unsigned)(*fmt++ - '0');
            if (width <= (2147483647u - d) / 10)
                width = width * 10 + d;
            else
                width = 2147483647u;
        }
        enum { LEN_INT, LEN_LONG, LEN_LLONG, LEN_SIZE } len = LEN_INT;
        if (*fmt == 'l') {
            len = LEN_LONG;
            if (*++fmt == 'l') {
                len = LEN_LLONG;
                ++fmt;
            }
        } else if (*fmt == 'z') {
            len = LEN_SIZE;
            ++fmt;
        }
        char conv = *fmt;
        if (!conv) {
            append(&o, "%", 1);
            break;
        }
        ++fmt;
        if (conv == 's' || conv == 'c' || conv == '%') {
            char c = conv == 'c' ? (char)va_arg(ap, int) : '%';
            const char *s = conv == 's' ? va_arg(ap, const char *) : &c;
            if (!s)
                s = "(null)";
            size_t n = conv == 's' ? strlen(s) : 1;
            size_t pad = width > n ? width - n : 0;
            if (!left)
                repeat(&o, ' ', pad);
            append(&o, s, n);
            if (left)
                repeat(&o, ' ', pad);
            continue;
        }
        if (conv != 'd' && conv != 'i' && conv != 'u' && conv != 'x' && conv != 'X' &&
            conv != 'p') {
            /* Unsupported conversions are visible and never consume a value. */
            append(&o, "%", 1);
            append(&o, &conv, 1);
            continue;
        }
        uint64_t value;
        const char *prefix = "";
        size_t prefix_len = 0;
        if (conv == 'p') {
            value = (uintptr_t)va_arg(ap, void *);
            prefix = "0x";
            prefix_len = 2;
        } else if (conv == 'd' || conv == 'i') {
            int64_t signed_value;
            switch (len) {
            case LEN_LONG:
                signed_value = va_arg(ap, long);
                break;
            case LEN_LLONG:
                signed_value = va_arg(ap, long long);
                break;
            case LEN_SIZE:
                signed_value = va_arg(ap, ptrdiff_t);
                break;
            default:
                signed_value = va_arg(ap, int);
                break;
            }
            value = (uint64_t)signed_value;
            if (signed_value < 0) {
                prefix = "-";
                prefix_len = 1;
                value = 0 - value;
            }
        } else {
            switch (len) {
            case LEN_LONG:
                value = va_arg(ap, unsigned long);
                break;
            case LEN_LLONG:
                value = va_arg(ap, unsigned long long);
                break;
            case LEN_SIZE:
                value = va_arg(ap, size_t);
                break;
            default:
                value = va_arg(ap, unsigned int);
                break;
            }
        }
        unsigned base = conv == 'x' || conv == 'X' || conv == 'p' ? 16 : 10;
        const char *digits = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        char number[64];
        size_t start = sizeof(number);
        do {
            number[--start] = digits[value % base];
            value /= base;
        } while (value);
        size_t n = sizeof(number) - start;
        size_t pad = width > n + prefix_len ? width - n - prefix_len : 0;
        if (!left && !zero)
            repeat(&o, ' ', pad);
        append(&o, prefix, prefix_len);
        if (!left && zero)
            repeat(&o, '0', pad);
        append(&o, number + start, n);
        if (left)
            repeat(&o, ' ', pad);
    }
    if (size)
        buf[o.count < size ? o.count : size - 1] = '\0';
    return o.count > 2147483647u ? 2147483647 : (int)o.count;
}

int vmm_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vmm_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static struct log_ring early_ring = {.magic = LOG_RING_MAGIC, .generation = 1};
static struct log_ring *ring = &early_ring;
static unsigned ring_lock, guest_lock;
static bool async_sinks, panic_mode;
static uint64_t sink_pos;
static uint8_t *mirror_flags;
static void (*fb_sink)(const char *, size_t);
static bool fb_enabled = true;
static unsigned fb_depth;
static enum log_level max_level = LOG_INFO;
/* Keep incomplete lines private until newline or shutdown. A line larger
 * than the ring cannot be retained in full; truncate it with a visible mark. */
#define GUEST_LINE_CAPACITY (LOG_RING_SIZE - 9u)
static char early_guest_line[LOG_RING_SIZE] = "[guest] ";
static char *guest_line = early_guest_line;
static bool guest_truncated;
static size_t guest_used;
/* A byte ring distinct from line-oriented firmware/log text (plan R-09).
 * An overrun is explicit in the transport, never silently normalised. */
static char early_serial[LOG_RING_SIZE];
static char *serial_data = early_serial;
static unsigned serial_lock;
static uint64_t serial_end, serial_sink_pos;
static bool serial_screen, serial_legacy_vga;

bool vmm_guest_serial_screen_enabled(void)
{
    return serial_screen && !serial_legacy_vga;
}
void vmm_guest_serial_configure(bool screen, bool legacy_vga)
{
    serial_screen = screen;
    serial_legacy_vga = legacy_vga;
    serial_end = serial_sink_pos = 0;
}

const struct log_ring *log_get_ring(void)
{
    return ring;
}
void log_set_fb_sink(void (*sink)(const char *, size_t))
{
    __atomic_store_n(&fb_sink, sink, __ATOMIC_RELEASE);
}
void log_set_level(enum log_level level)
{
    __atomic_store_n(&max_level, level, __ATOMIC_RELEASE);
}
bool vmm_log_fb_enabled(void)
{
    return __atomic_load_n(&fb_enabled, __ATOMIC_ACQUIRE);
}
void vmm_log_fb_enable(bool enable)
{
    bool old = __atomic_exchange_n(&fb_enabled, enable, __ATOMIC_ACQ_REL);
    if (enable && !old && hal_cpu_index() == 0)
        vmm_guest_serial_show_tail();
}

static void lock(unsigned *p)
{
    while (__atomic_exchange_n(p, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static void unlock(unsigned *p)
{
    __atomic_store_n(p, 0, __ATOMIC_RELEASE);
}
size_t vmm_guest_serial_read(uint64_t *cursor, char *data, size_t size, uint64_t *lost)
{
    *lost = 0;
    if (__atomic_load_n(&panic_mode, __ATOMIC_ACQUIRE)) {
        if (__atomic_exchange_n(&serial_lock, 1, __ATOMIC_ACQUIRE))
            return 0;
    } else
        lock(&serial_lock);
    uint64_t first = serial_end > LOG_RING_SIZE ? serial_end - LOG_RING_SIZE : 0;
    *lost = *cursor < first ? first - *cursor : 0;
    if (*cursor < first)
        *cursor = first;
    size_t n = *cursor < serial_end ? serial_end - *cursor : 0;
    if (n > size)
        n = size;
    for (size_t i = 0; i < n; ++i)
        data[i] = serial_data[(*cursor + i) % LOG_RING_SIZE];
    *cursor += n;
    unlock(&serial_lock);
    return n;
}
void vmm_guest_serial_write(const char *s, size_t n)
{
    lock(&serial_lock);
    for (size_t i = 0; i < n; ++i)
        serial_data[serial_end++ % LOG_RING_SIZE] = s[i];
    unlock(&serial_lock);
    /* Waking management is bounded; slow physical UART/FB sinks run there. */
    if (async_sinks && hal_cpu_index() != 0)
        hal_cpu_request(0, HAL_CPU_WAKE);
    vmm_log_guest_text(s, n);
}
static bool serial_drain(void)
{
    char bytes[256], frame[560];
    uint64_t lost;
    size_t n = vmm_guest_serial_read(&serial_sink_pos, bytes, sizeof(bytes), &lost);
    if (lost) {
        int count = vmm_snprintf(frame, sizeof(frame), "\n[vmm-serial-lost] %llu\n",
                                 (unsigned long long)lost);
        hal_debug_write(frame, (size_t)count);
    }
    if (!n)
        return false;
    static const char digits[] = "0123456789abcdef";
    size_t at = (size_t)vmm_snprintf(frame, sizeof(frame), "\n[vmm-serial] %016llx %04x ",
                                     (unsigned long long)(serial_sink_pos - n), (unsigned)n);
    for (size_t i = 0; i < n; ++i) {
        frame[at++] = digits[(uint8_t)bytes[i] >> 4];
        frame[at++] = digits[(uint8_t)bytes[i] & 15];
    }
    frame[at++] = '\n';
    hal_debug_write(frame, at);
    void (*sink)(const char *, size_t) = __atomic_load_n(&fb_sink, __ATOMIC_ACQUIRE);
    if (sink && vmm_guest_serial_screen_enabled() &&
        !__atomic_load_n(&fb_enabled, __ATOMIC_ACQUIRE))
        sink(bytes, n);
    return true;
}
void vmm_guest_serial_show_tail(void)
{
    if (hal_cpu_index() != 0)
        return;
    char bytes[2048];
    uint64_t cursor, lost;
    if (__atomic_load_n(&panic_mode, __ATOMIC_ACQUIRE)) {
        if (__atomic_exchange_n(&serial_lock, 1, __ATOMIC_ACQUIRE))
            return;
    } else
        lock(&serial_lock);
    /* Several exit paths re-enable the console; show each tail once. */
    static uint64_t shown = UINT64_MAX;
    bool fresh = serial_end != shown;
    shown = serial_end;
    cursor = serial_end > sizeof(bytes) ? serial_end - sizeof(bytes) : 0;
    unlock(&serial_lock);
    if (!fresh)
        return;
    size_t n = vmm_guest_serial_read(&cursor, bytes, sizeof(bytes), &lost);
    void (*sink)(const char *, size_t) = __atomic_load_n(&fb_sink, __ATOMIC_ACQUIRE);
    if (sink && n) {
        sink("\nGuest serial tail:\n", 20);
        sink(bytes, n);
    }
}
void log_panic_begin(void)
{
    __atomic_store_n(&panic_mode, true, __ATOMIC_RELEASE);
}

size_t log_ring_since(const struct log_ring *r, uint64_t from, char (*lines)[LOG_MATCH_COLUMNS + 1],
                      size_t max, size_t *total)
{
    uint64_t end = r->write_pos, first = end > LOG_RING_SIZE ? end - LOG_RING_SIZE : 0;
    size_t count = 0, used = 0;
    *total = 0;
    if (!max)
        return 0;
    uint64_t at = from > first ? from : first;
    /* A start inside a line (the ring wrapped past `from`): skip that fragment. */
    if (at != from)
        while (at < end && r->data[at++ % LOG_RING_SIZE] != '\n') {
        }
    for (; at < end; ++at) {
        char c = r->data[at % LOG_RING_SIZE], *line = lines[count % max];
        if (c == '\n') {
            line[used < LOG_MATCH_COLUMNS ? used : LOG_MATCH_COLUMNS] = 0;
            ++count;
            used = 0;
            continue;
        }
        if (used < LOG_MATCH_COLUMNS)
            line[used] = c;
        ++used;
    }
    *total = count;
    if (count <= max)
        return count;
    /* Rotate the ring of slots so the oldest kept line comes first. */
    size_t start = count % max;
    char swap[LOG_MATCH_COLUMNS + 1];
    size_t spans[3][2] = {{0, start}, {start, max}, {0, max}};
    for (unsigned k = 0; k < 3; ++k)
        for (size_t i = spans[k][0], j = spans[k][1]; i + 1 < j; ++i, --j) {
            memcpy(swap, lines[i], sizeof(swap));
            memcpy(lines[i], lines[j - 1], sizeof(swap));
            memcpy(lines[j - 1], swap, sizeof(swap));
        }
    return max;
}

size_t log_ring_matching(const struct log_ring *r, const char *prefix,
                         char (*lines)[LOG_MATCH_COLUMNS + 1], size_t max, size_t *kept)
{
    size_t total = 0, used = 0, plen = strlen(prefix);
    bool match = true;
    uint64_t end = r->write_pos, at = end > LOG_RING_SIZE ? end - LOG_RING_SIZE : 0;
    /* A wrapped ring starts inside a line: skip that fragment. */
    if (at)
        while (at < end && r->data[at++ % LOG_RING_SIZE] != '\n') {
        }
    for (; max && at < end; ++at) {
        char c = r->data[at % LOG_RING_SIZE], *line = lines[total % max];
        if (c == '\n') {
            if (match && used >= plen) {
                line[used < LOG_MATCH_COLUMNS ? used : LOG_MATCH_COLUMNS] = 0;
                ++total;
            }
            used = 0;
            match = true;
            continue;
        }
        /* Before the prefix is complete only equal characters are stored,
         * so a rejected line never alters the oldest kept line in its slot. */
        if (used < plen && c != prefix[used])
            match = false;
        if (match && used < LOG_MATCH_COLUMNS)
            line[used] = c;
        ++used;
    }
    *kept = total < max ? total : max;
    /* The slots form a ring once more than max lines matched: rotate the
     * oldest kept line to index 0 by three reversals. */
    if (total > max) {
        size_t first = total % max;
        char swap[LOG_MATCH_COLUMNS + 1];
        size_t spans[3][2] = {{0, first}, {first, max}, {0, max}};
        for (unsigned s = 0; s < 3; ++s)
            for (size_t i = spans[s][0], j = spans[s][1]; i + 1 < j; ++i, --j) {
                memcpy(swap, lines[i], sizeof(swap));
                memcpy(lines[i], lines[j - 1], sizeof(swap));
                memcpy(lines[j - 1], swap, sizeof(swap));
            }
    }
    return total;
}

void log_relocate(void)
{
    struct log_ring *owned = hal_alloc_pages(IA64VMM_ALIGN_UP(sizeof(*ring), 4096) / 4096, 0);
    char *line = hal_alloc_pages(LOG_RING_SIZE / 4096, 0);
    char *serial = hal_alloc_pages(LOG_RING_SIZE / 4096, 0);
    mirror_flags = hal_alloc_pages(LOG_RING_SIZE / 4096, HAL_PAGE_ZERO);
    if (!owned || !line || !serial || !mirror_flags)
        hal_panic("cannot relocate RAM log");
    memcpy(owned, ring, sizeof(*ring));
    memcpy(line, guest_line, LOG_RING_SIZE);
    ring = owned;
    guest_line = line;
    memcpy(serial, serial_data, LOG_RING_SIZE);
    serial_data = serial;
}
void log_set_async(void)
{
    if (!mirror_flags)
        hal_panic("asynchronous log requires relocated storage");
    sink_pos = ring->write_pos;
    async_sinks = true;
}
static void mirrors(const char *s, size_t n, uint8_t flags)
{
    if (flags & 1)
        hal_debug_write(s, n);
    void (*sink)(const char *, size_t) = __atomic_load_n(&fb_sink, __ATOMIC_ACQUIRE);
    if ((flags & 2) && sink && !fb_depth) {
        __atomic_store_n(&fb_depth, 1, __ATOMIC_RELEASE);
        sink(s, n);
        __atomic_store_n(&fb_depth, 0, __ATOMIC_RELEASE);
    }
}
void log_drain(void)
{
    static bool draining;
    if (hal_cpu_index() != 0 || draining)
        return;
    draining = true;
    /* Drain a finite snapshot-sized batch. Producers never wait for sinks;
     * the next wake drains further bytes if a guest floods the ring. */
    for (unsigned i = 0; i < LOG_RING_SIZE / 256 && serial_drain(); ++i) {
    }
    if (!async_sinks) {
        draining = false;
        return;
    }
    for (;;) {
        char text[2048];
        uint8_t flags[2048];
        if (__atomic_load_n(&panic_mode, __ATOMIC_ACQUIRE)) {
            if (__atomic_exchange_n(&ring_lock, 1, __ATOMIC_ACQUIRE))
                break;
        } else
            lock(&ring_lock);
        uint64_t end = ring->write_pos;
        uint64_t lost = end - sink_pos > LOG_RING_SIZE ? end - sink_pos - LOG_RING_SIZE : 0;
        sink_pos += lost;
        size_t n = end - sink_pos;
        if (n > sizeof(text))
            n = sizeof(text);
        for (size_t i = 0; i < n; ++i) {
            unsigned at = (sink_pos + i) % LOG_RING_SIZE;
            text[i] = ring->data[at];
            flags[i] = mirror_flags[at];
        }
        sink_pos += n;
        unlock(&ring_lock);
        if (lost) {
            char report[96];
            int count =
                vmm_snprintf(report, sizeof(report), "[warn] log mirrors overrun: %llu bytes\n",
                             (unsigned long long)lost);
            hal_debug_write(report, (size_t)count);
        }
        if (!n)
            break;
        for (size_t i = 0; i < n;) {
            size_t end_run = i + 1;
            while (end_run < n && flags[end_run] == flags[i])
                ++end_run;
            mirrors(text + i, end_run - i, flags[i]);
            i = end_run;
        }
    }
    draining = false;
}
static void emit(const char *s, size_t n, enum log_level level)
{
    /* Only the ring commit is locked. Formatting and slow sink calls never
     * run under this lock. Panic may have interrupted its owner: bypass
     * rather than waiting for a CPU that cannot release it. */
    if (__atomic_load_n(&panic_mode, __ATOMIC_ACQUIRE)) {
        if (__atomic_exchange_n(&ring_lock, 1, __ATOMIC_ACQUIRE)) {
            hal_debug_write(s, n);
            return;
        }
    } else
        lock(&ring_lock);
    uint8_t flags = level <= __atomic_load_n(&max_level, __ATOMIC_ACQUIRE) ? 1 : 0;
    if (flags && __atomic_load_n(&fb_enabled, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&fb_depth, __ATOMIC_ACQUIRE))
        flags |= 2;
    for (size_t i = 0; i < n; ++i) {
        unsigned at = ring->write_pos++ % LOG_RING_SIZE;
        ring->data[at] = s[i];
        if (mirror_flags)
            mirror_flags[at] = flags;
    }
    unlock(&ring_lock);
    if (async_sinks) {
        /* RAM-only lines (below the mirror level) need no drain now; the
         * next mirrored line or shutdown drains past them. */
        if (hal_cpu_index() == 0)
            log_drain();
        else if (flags)
            hal_cpu_request(0, HAL_CPU_WAKE);
    } else
        mirrors(s, n, flags);
}

void vmm_vlog(enum log_level level, const char *fmt, va_list ap)
{
    static const char *const tags[] = {"[error] ", "[warn] ", "[info] ", "[debug] "};
    if ((unsigned)level > LOG_DEBUG)
        return;
    char text[2048];
    size_t tag_len = strlen(tags[level]);
    memcpy(text, tags[level], tag_len);
    int wanted = vmm_vsnprintf(text + tag_len, sizeof(text) - tag_len - 1, fmt, ap);
    size_t used = (size_t)wanted;
    if (used > sizeof(text) - tag_len - 2)
        used = sizeof(text) - tag_len - 2;
    text[tag_len + used] = '\n';
    emit(text, tag_len + used + 1, level);
}

void vmm_log(enum log_level level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmm_vlog(level, fmt, ap);
    va_end(ap);
}

static void guest_flush(bool empty_line)
{
    if (!guest_used && !empty_line)
        return;
    if (guest_truncated) {
        static const char mark[] = " [truncated]";
        memcpy(guest_line + 8 + guest_used - (sizeof(mark) - 1), mark, sizeof(mark) - 1);
    }
    size_t n = 8 + guest_used;
    guest_line[n++] = '\n';
    guest_used = 0;
    guest_truncated = false;
    emit(guest_line, n, LOG_INFO);
}

static void (*guest_line_hook)(const char *line, size_t n);

void vmm_log_set_guest_line_hook(void (*hook)(const char *line, size_t n))
{
    guest_line_hook = hook;
}

void vmm_log_guest_text(const char *s, size_t n)
{
    lock(&guest_lock);
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\r')
            continue;
        if (s[i] == '\n') {
            char line[64];
            size_t len = guest_used < sizeof(line) ? guest_used : sizeof(line);
            if (guest_line_hook)
                memcpy(line, guest_line + 8, len);
            guest_flush(true);
            if (guest_line_hook)
                guest_line_hook(line, len);
            continue;
        }
        if (guest_used == GUEST_LINE_CAPACITY) {
            guest_truncated = true;
            continue;
        }
        guest_line[8 + guest_used++] = s[i];
    }
    unlock(&guest_lock);
}

void log_flush_guest_text(void)
{
    if (__atomic_load_n(&panic_mode, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&guest_lock, __ATOMIC_ACQUIRE))
        return;
    lock(&guest_lock);
    guest_flush(false);
    unlock(&guest_lock);
    log_drain();
    /* Final drain has no future management wake; retain the exact suffix. */
    if (hal_cpu_index() == 0)
        while (serial_drain()) {
        }
    if (hal_cpu_index() == 0 && !__atomic_load_n(&panic_mode, __ATOMIC_ACQUIRE)) {
        lock(&serial_lock);
        uint64_t end = serial_end;
        unlock(&serial_lock);
        char checkpoint[64];
        int n = vmm_snprintf(checkpoint, sizeof(checkpoint), "\n[vmm-serial-end] %016llx\n",
                             (unsigned long long)end);
        hal_debug_write(checkpoint, (size_t)n);
    }
}

/* Warm-reset recovery is best effort: UEFI allocations or a changed memory
 * encryption key may have destroyed the prior ring before we reach it. */
static struct log_ring *crash_ring;
bool log_in_panic(void)
{
    return __atomic_load_n(&panic_mode, __ATOMIC_ACQUIRE);
}
void log_relocate_fixed(void *storage)
{
    struct log_ring *prior = storage;
    crash_ring = NULL;
    if (prior->magic == LOG_RING_MAGIC && prior->generation &&
        prior->write_pos >= prior->flushed_pos) {
        crash_ring = hal_alloc_pages(IA64VMM_ALIGN_UP(sizeof(*prior), 4096) / 4096, 0);
        if (!crash_ring)
            hal_panic("cannot retain warm-reset crash ring");
        memcpy(crash_ring, prior, sizeof(*prior));
    }
    log_relocate();
    if (crash_ring)
        ring->generation = crash_ring->generation == UINT64_MAX ? 1 : crash_ring->generation + 1;
    memcpy(prior, ring, sizeof(*prior));
    ring = prior;
    if (crash_ring)
        LOGI("PLOG recovered warm-reset RAM ring generation=%llu bytes=%llu",
             (unsigned long long)crash_ring->generation, (unsigned long long)crash_ring->write_pos);
    else
        LOGI(
            "PLOG no valid prior RAM ring; cold boot, firmware reuse or memory encryption may have erased it");
}
size_t log_media_read(uint64_t *cursor, void *data, size_t size, uint64_t *lost)
{
    *lost = 0;
    if (log_in_panic())
        return 0;
    lock(&ring_lock);
    uint64_t first = ring->write_pos > LOG_RING_SIZE ? ring->write_pos - LOG_RING_SIZE : 0;
    if (*cursor < first) {
        *lost = first - *cursor;
        *cursor = first;
    }
    size_t n = *cursor < ring->write_pos ? ring->write_pos - *cursor : 0;
    if (n > size)
        n = size;
    for (size_t i = 0; i < n; ++i)
        ((char *)data)[i] = ring->data[(*cursor + i) % LOG_RING_SIZE];
    *cursor += n;
    unlock(&ring_lock);
    return n;
}
void log_media_flushed(uint64_t position)
{
    lock(&ring_lock);
    if (position <= ring->write_pos && position > ring->flushed_pos)
        ring->flushed_pos = position;
    unlock(&ring_lock);
}
size_t log_crash_read(uint64_t *cursor, void *data, size_t size)
{
    if (!crash_ring)
        return 0;
    uint64_t first =
        crash_ring->write_pos > LOG_RING_SIZE ? crash_ring->write_pos - LOG_RING_SIZE : 0;
    if (*cursor < first)
        *cursor = first;
    size_t n = *cursor < crash_ring->write_pos ? crash_ring->write_pos - *cursor : 0;
    if (n > size)
        n = size;
    for (size_t i = 0; i < n; ++i)
        ((char *)data)[i] = crash_ring->data[(*cursor + i) % LOG_RING_SIZE];
    *cursor += n;
    return n;
}
void log_media_clean(void)
{
    if (!log_in_panic())
        ring->magic = 0;
}

uint64_t log_media_end(void)
{
    if (log_in_panic())
        return 0;
    lock(&ring_lock);
    uint64_t end = ring->write_pos;
    unlock(&ring_lock);
    return end;
}
