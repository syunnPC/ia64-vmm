/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Root files on a reserved FAT32 medium, with 512-byte logical sectors. */
#include "persist/fat32.h"
#include "ia64vmm/string.h"
#define EOC 0x0ffffff8u
#define MASK 0x0fffffffu
#define UNKNOWN UINT32_MAX
static uint16_t u16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}
static uint32_t u32(const uint8_t *p)
{
    return u16(p) | (uint32_t)u16(p + 2) << 16;
}
static void p16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}
static void p32(uint8_t *p, uint32_t v)
{
    p16(p, v);
    p16(p + 2, v >> 16);
}
static bool bad(struct fat32 *f)
{
    f->failed = true;
    return false;
}
static bool cluster(const struct fat32 *f, uint32_t c)
{
    return c >= 2 && c - 2 < f->clusters;
}
static uint64_t lba(const struct fat32 *f, uint32_t c)
{
    return f->data_lba + (uint64_t)(c - 2) * f->sectors_per_cluster;
}
static bool rd(struct fat32 *f, uint64_t at, unsigned n, void *out)
{
    if (f->failed || !n || n > 32 || at < f->first || at > f->last || n > f->last - at + 1)
        return bad(f);
    return f->block->read(at, n, out) || bad(f);
}
static bool wr(struct fat32 *f, uint64_t at, unsigned n, const void *in)
{
    if (f->failed || !n || n > 32 || at < f->first || at > f->last || n > f->last - at + 1)
        return bad(f);
    return f->block->write(at, n, in) || bad(f);
}
bool fat32_flush(struct fat32 *f)
{
    return !f->failed && (f->block->flush() || bad(f));
}
static bool fat_get(struct fat32 *f, uint32_t c, uint32_t *v)
{
    if (c > f->clusters + 1)
        return bad(f);
    uint64_t at = f->fat_lba + (uint64_t)f->active_fat * f->fat_sectors + c / 128;
    if (f->cached_fat_lba != at) {
        if (!rd(f, at, 1, f->fat_work))
            return false;
        if (f->mirrored)
            for (unsigned i = 1; i < f->fat_count; ++i) {
                if (!rd(f, f->fat_lba + (uint64_t)i * f->fat_sectors + c / 128, 1, f->work))
                    return false;
                for (unsigned j = 0; j < 512; j += 4)
                    if ((u32(f->work + j) & MASK) != (u32(f->fat_work + j) & MASK))
                        return bad(f);
            }
        f->cached_fat_lba = at;
    }
    *v = u32(f->fat_work + (size_t)(c % 128) * 4) & MASK;
    return true;
}
static bool fat_set(struct fat32 *f, uint32_t c, uint32_t v)
{
    uint32_t old;
    if (!cluster(f, c) || !fat_get(f, c, &old))
        return bad(f);
    f->cached_fat_lba = UINT64_MAX;
    unsigned start = f->mirrored ? 0 : f->active_fat;
    unsigned end = f->mirrored ? f->fat_count : start + 1;
    for (unsigned i = start; i < end; ++i) {
        uint64_t at = f->fat_lba + (uint64_t)i * f->fat_sectors + c / 128;
        if (!rd(f, at, 1, f->work))
            return false;
        uint8_t *p = f->work + (size_t)(c % 128) * 4;
        p32(p, (u32(p) & ~MASK) | v);
        if (!wr(f, at, 1, f->work))
            return false;
    }
    return true;
}
/* Brent's cycle detector uses one FAT lookup per link and constant memory. */
static bool chain(struct fat32 *f, uint32_t first, uint32_t *count, uint32_t *tail)
{
    uint32_t c = first, anchor = first, power = 1, length = 0;
    *count = 0;
    *tail = 0;
    if (!first)
        return true;
    for (;;) {
        uint32_t next;
        if (!cluster(f, c) || ++*count > f->clusters || !fat_get(f, c, &next))
            return bad(f);
        if (next >= EOC) {
            *tail = c;
            return true;
        }
        if (!cluster(f, next))
            return bad(f);
        c = next;
        if (c == anchor)
            return bad(f);
        if (++length == power) {
            anchor = c;
            length = 0;
            if (power <= f->clusters / 2)
                power *= 2;
        }
    }
}
static bool info_valid(const uint8_t *b)
{
    return u32(b) == 0x41615252 && u32(b + 484) == 0x61417272 && u32(b + 508) == 0xaa550000;
}
bool fat32_mount(struct fat32 *f, const struct plog_block *b, uint64_t first, uint64_t last)
{
    memset(f, 0, sizeof(*f));
    f->failed = true;
    if (!b || !b->read || !b->write || !b->flush || first > last || last >= b->sectors)
        return false;
    f->block = b;
    f->first = first;
    f->last = last;
    f->failed = false;
    f->cached_fat_lba = UINT64_MAX;
    if (!rd(f, first, 1, f->work))
        return false;
    const uint8_t *p = f->work;
    uint16_t flags = u16(p + 40);
    f->sectors_per_cluster = p[13];
    f->reserved = u16(p + 14);
    f->fat_count = p[16];
    f->total_sectors = u32(p + 32);
    f->fat_sectors = u32(p + 36);
    f->root_cluster = u32(p + 44);
    f->fsinfo = u16(p + 48);
    f->mirrored = !(flags & 0x80);
    f->active_fat = f->mirrored ? 0 : flags & 15;
    uint64_t overhead = f->reserved + (uint64_t)f->fat_count * f->fat_sectors;
    unsigned spc = f->sectors_per_cluster;
    if (u16(p + 510) != 0xaa55 || u16(p + 11) != 512 || !spc || spc > 128 || (spc & (spc - 1)) ||
        !f->reserved || !f->fat_count || !f->fat_sectors || u16(p + 17) || u16(p + 19) ||
        u16(p + 22) || u16(p + 42) || (flags & 0xff70) || f->active_fat >= f->fat_count ||
        f->total_sectors <= overhead || f->total_sectors > last - first + 1)
        return bad(f);
    f->clusters = (f->total_sectors - overhead) / spc;
    if (f->clusters < 65525 || f->clusters > 0x0fffffee ||
        (uint64_t)(f->clusters + 2) * 4 > (uint64_t)f->fat_sectors * 512 ||
        !cluster(f, f->root_cluster))
        return bad(f);
    f->last = first + f->total_sectors - 1;
    f->fat_lba = first + f->reserved;
    f->data_lba = first + overhead;
    f->free_count = f->next_free = UNKNOWN;
    if (f->fsinfo && f->fsinfo != 0xffff) {
        if (f->fsinfo >= f->reserved || !rd(f, first + f->fsinfo, 1, f->work))
            return bad(f);
        if (info_valid(f->work)) {
            f->free_count = u32(f->work + 488);
            f->next_free = u32(f->work + 492);
            if (f->free_count > f->clusters)
                f->free_count = UNKNOWN;
            if (!cluster(f, f->next_free))
                f->next_free = UNKNOWN;
        } else
            f->fsinfo = 0;
    } else
        f->fsinfo = 0;
    uint32_t count;
    return chain(f, f->root_cluster, &count, &f->root_tail);
}
static unsigned upper(unsigned c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}
static bool equal(const char *a, const char *b)
{
    for (unsigned i = 0; i < 256; ++i) {
        if (upper((uint8_t)a[i]) != upper((uint8_t)b[i]))
            return false;
        if (!a[i])
            return true;
    }
    return false;
}
static bool name_valid(const char *s)
{
    for (unsigned i = 0; i < 256; ++i) {
        unsigned c = (uint8_t)s[i];
        if (!c)
            return i && s[i - 1] != ' ' && s[i - 1] != '.';
        if (c < 32 || c >= 127 || c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' ||
            c == '?' || c == '<' || c == '>' || c == '|')
            return false;
    }
    return false;
}
static bool sfn(const uint8_t *e, const char *name)
{
    char s[13];
    unsigned n = 0, end = 8;
    while (end && e[end - 1] == ' ')
        --end;
    for (unsigned i = 0; i < end; ++i) {
        if (e[i] < 32 || e[i] >= 127)
            return false;
        s[n++] = (char)e[i];
    }
    end = 3;
    while (end && e[8 + end - 1] == ' ')
        --end;
    if (end) {
        s[n++] = '.';
        for (unsigned i = 0; i < end; ++i)
            s[n++] = (char)e[8 + i];
    }
    s[n] = 0;
    return equal(s, name);
}
static uint8_t checksum(const uint8_t *s)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; ++i)
        sum = (sum >> 1) + ((sum & 1) << 7) + s[i];
    return sum;
}
struct long_name {
    char name[260];
    unsigned next, sum, slots;
    bool valid;
};
static void long_entry(struct long_name *n, const uint8_t *e)
{
    static const uint8_t offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    unsigned seq = e[0] & 31;
    if (e[0] & 0x40) {
        memset(n, 0, sizeof(*n));
        n->next = seq;
        n->sum = e[13];
        n->slots = seq;
        n->valid = seq && seq <= 20;
    }
    if (!n->valid || (e[0] & 0xa0) || !seq || seq != n->next || e[13] != n->sum || e[12] ||
        u16(e + 26)) {
        n->valid = false;
        return;
    }
    for (unsigned i = 0; i < 13; ++i) {
        unsigned at = (seq - 1) * 13 + i, c = u16(e + offsets[i]);
        if (c && c != 0xffff && (c < 32 || c >= 127 || at >= 255)) {
            n->valid = false;
            return;
        }
        n->name[at] = (char)(c == 0xffff ? 0xff : c);
    }
    --n->next;
}
static bool long_match(struct long_name *n, const uint8_t *e, const char *name)
{
    if (!n->valid || n->next || checksum(e) != n->sum)
        return false;
    unsigned len = n->slots * 13, end = len;
    for (unsigned i = 0; i < len; ++i)
        if (!n->name[i]) {
            end = i;
            break;
        }
    if (!end || end > 255 || end <= (n->slots - 1) * 13)
        return false;
    for (unsigned i = 0; i < end; ++i)
        if ((uint8_t)n->name[i] == 255)
            return false;
    for (unsigned i = end + 1; i < len; ++i)
        if ((uint8_t)n->name[i] != 255)
            return false;
    n->name[end] = 0;
    return name_valid(n->name) && equal(n->name, name);
}
static bool load_file(struct fat32 *f, struct fat32_file *file, const uint8_t *e, uint64_t at,
                      unsigned off)
{
    memset(file, 0, sizeof(*file));
    file->dir_lba = at;
    file->dir_offset = off;
    file->first_cluster = (uint32_t)u16(e + 20) << 16 | u16(e + 26);
    file->size = u32(e + 28);
    file->attributes = e[11];
    if ((file->first_cluster & ~MASK) || (!file->first_cluster && file->size) ||
        !chain(f, file->first_cluster, &file->chain_clusters, &file->tail_cluster))
        return bad(f);
    uint64_t cluster_bytes = (uint64_t)f->sectors_per_cluster * 512;
    uint32_t need = (uint32_t)(((uint64_t)file->size + cluster_bytes - 1) / cluster_bytes);
    if (file->chain_clusters < need || (file->tail_cluster && file->tail_cluster == f->root_tail))
        return bad(f);
    return true;
}
bool fat32_disjoint(const struct fat32_file *a, const struct fat32_file *b)
{
    return !a->tail_cluster || !b->tail_cluster || a->tail_cluster != b->tail_cluster;
}
static bool lookup(struct fat32 *f, const char *name, struct fat32_file *file, bool existence)
{
    if (f->failed || !name_valid(name))
        return false;
    f->cached_fat_lba = UINT64_MAX;
    uint32_t count, tail;
    if (!chain(f, f->root_cluster, &count, &tail) || tail != f->root_tail)
        return bad(f);
    struct long_name n;
    memset(&n, 0, sizeof(n));
    uint32_t c = f->root_cluster;
    for (uint32_t visited = 0; visited < count; ++visited) {
        for (unsigned s = 0; s < f->sectors_per_cluster; ++s) {
            uint64_t at = lba(f, c) + s;
            if (!rd(f, at, 1, f->work))
                return false;
            for (unsigned off = 0; off < 512; off += 32) {
                const uint8_t *e = f->work + off;
                if (!e[0])
                    return false;
                if (e[0] == 0xe5) {
                    n.valid = false;
                    continue;
                }
                if (e[11] == 0x0f) {
                    long_entry(&n, e);
                    continue;
                }
                bool match = (existence || !(e[11] & 0x18)) && !(e[11] & 0xc0) &&
                             (sfn(e, name) || long_match(&n, e, name));
                n.valid = false;
                if (match) {
                    if (existence)
                        return true;
                    uint8_t copy[32];
                    memcpy(copy, e, sizeof(copy));
                    return load_file(f, file, copy, at, off);
                }
            }
        }
        if (!fat_get(f, c, &c))
            return false;
    }
    return false;
}
bool fat32_lookup(struct fat32 *f, const char *name, struct fat32_file *file)
{
    return lookup(f, name, file, false);
}
static bool metadata(struct fat32 *f, const struct fat32_file *file)
{
    if (file->dir_offset > 480 || file->dir_offset % 32 || !rd(f, file->dir_lba, 1, f->work))
        return bad(f);
    const uint8_t *e = f->work + file->dir_offset;
    if (!e[0] || e[0] == 0xe5 || e[11] != file->attributes || (e[11] & 0xd8) ||
        ((uint32_t)u16(e + 20) << 16 | u16(e + 26)) != file->first_cluster ||
        u32(e + 28) != file->size)
        return bad(f);
    return true;
}
static bool at_cluster(struct fat32 *f, const struct fat32_file *file, uint32_t index,
                       uint32_t *out)
{
    if (index >= file->chain_clusters)
        return bad(f);
    if (f->cursor_first != file->first_cluster || f->cursor_index > index || !f->cursor_cluster) {
        f->cursor_first = file->first_cluster;
        f->cursor_cluster = file->first_cluster;
        f->cursor_index = 0;
    }
    while (f->cursor_index < index) {
        if (!fat_get(f, f->cursor_cluster, &f->cursor_cluster) || !cluster(f, f->cursor_cluster))
            return bad(f);
        ++f->cursor_index;
    }
    *out = f->cursor_cluster;
    return cluster(f, *out) || bad(f);
}
static bool transfer(struct fat32 *f, const struct fat32_file *file, uint32_t offset, void *buf,
                     uint32_t bytes, bool writing)
{
    if (f->failed || offset > file->size || bytes > file->size - offset ||
        (writing && (file->attributes & 1)))
        return false;
    if (!metadata(f, file))
        return false;
    uint8_t *p = buf;
    uint32_t cb = f->sectors_per_cluster * 512u;
    while (bytes) {
        uint32_t c;
        if (!at_cluster(f, file, offset / cb, &c))
            return false;
        unsigned within = offset % cb, sector_off = within % 512;
        uint64_t at = lba(f, c) + within / 512;
        unsigned n = 512 - sector_off;
        if (n > bytes)
            n = bytes;
        if (sector_off || n < 512) {
            if (!rd(f, at, 1, f->work))
                return false;
            if (writing) {
                memcpy(f->work + sector_off, p, n);
                if (!wr(f, at, 1, f->work))
                    return false;
            } else
                memcpy(p, f->work + sector_off, n);
        } else {
            unsigned limit = bytes / 512, sectors = f->sectors_per_cluster - within / 512;
            if (limit > 32)
                limit = 32;
            if (sectors > limit)
                sectors = limit;
            uint32_t last = c, index = offset / cb;
            while (sectors < limit && index + 1 < file->chain_clusters) {
                uint32_t next;
                if (!fat_get(f, last, &next) || !cluster(f, next))
                    return bad(f);
                if (next != last + 1)
                    break;
                unsigned add = limit - sectors;
                if (add > f->sectors_per_cluster)
                    add = f->sectors_per_cluster;
                sectors += add;
                last = next;
                ++index;
                f->cursor_cluster = last;
                f->cursor_index = index;
            }
            n = sectors * 512;
            if (writing ? !wr(f, at, sectors, p) : !rd(f, at, sectors, p))
                return false;
        }
        p += n;
        offset += n;
        bytes -= n;
    }
    return true;
}
bool fat32_read(struct fat32 *f, const struct fat32_file *file, uint32_t at, void *out,
                uint32_t bytes)
{
    return transfer(f, file, at, out, bytes, false);
}
bool fat32_write(struct fat32 *f, const struct fat32_file *file, uint32_t at, const void *in,
                 uint32_t bytes)
{
    return transfer(f, file, at, (void *)in, bytes, true);
}
/* Publish unknown hints before changing allocation. A stale hint must never
 * advertise a free cluster after an interrupted metadata update. */
static bool info_write(struct fat32 *f, uint32_t count, uint32_t next)
{
    if (!f->fsinfo)
        return true;
    uint64_t at = f->first + f->fsinfo;
    if (!rd(f, at, 1, f->work) || !info_valid(f->work))
        return bad(f);
    p32(f->work + 488, count);
    p32(f->work + 492, next);
    return wr(f, at, 1, f->work);
}
static bool allocate(struct fat32 *f, uint32_t *out)
{
    uint32_t c = cluster(f, f->next_free) ? f->next_free : 2;
    for (uint32_t i = 0; i < f->clusters; ++i) {
        uint32_t value;
        if (!fat_get(f, c, &value))
            return false;
        if (!value) {
            /* Reserve the cluster before linking it to any live chain. */
            if (!fat_set(f, c, MASK))
                return false;
            memset(f->work, 0, 512);
            for (unsigned s = 0; s < f->sectors_per_cluster; ++s)
                if (!wr(f, lba(f, c) + s, 1, f->work))
                    return false;
            if (!fat32_flush(f))
                return false;
            f->next_free = c == f->clusters + 1 ? 2 : c + 1;
            if (f->free_count != UNKNOWN) {
                if (f->free_count)
                    --f->free_count;
                else
                    f->free_count = UNKNOWN;
            }
            *out = c;
            return true;
        }
        c = c == f->clusters + 1 ? 2 : c + 1;
    }
    return false;
}
static bool publish(struct fat32 *f, const struct fat32_file *old, const struct fat32_file *file)
{
    if (!metadata(f, old))
        return false;
    uint8_t *e = f->work + old->dir_offset;
    p16(e + 20, file->first_cluster >> 16);
    p16(e + 26, file->first_cluster);
    p32(e + 28, file->size);
    e[11] |= 0x20;
    return wr(f, old->dir_lba, 1, f->work) && fat32_flush(f);
}
bool fat32_grow(struct fat32 *f, struct fat32_file *file, uint32_t size)
{
    if (f->failed || size < file->size || (file->attributes & 1))
        return false;
    if (!metadata(f, file))
        return false;
    if (size == file->size)
        return true;
    struct fat32_file grown = *file;
    uint32_t cb = f->sectors_per_cluster * 512u;
    uint32_t need = (uint32_t)(((uint64_t)size + cb - 1) / cb);
    bool allocating = need > file->chain_clusters;
    if (allocating && (!info_write(f, UNKNOWN, UNKNOWN) || !fat32_flush(f)))
        return false;
    while (grown.chain_clusters < need) {
        uint32_t c;
        if (!allocate(f, &c))
            return bad(f);
        if (grown.tail_cluster && !fat_set(f, grown.tail_cluster, c))
            return false;
        if (!grown.first_cluster)
            grown.first_cluster = c;
        grown.tail_cluster = c;
        ++grown.chain_clusters;
    }
    f->cursor_first = 0;
    /* Zero the old last cluster's slack and any preallocated excess too. */
    uint32_t pos = file->size;
    while (pos < size) {
        uint32_t c;
        if (!at_cluster(f, &grown, pos / cb, &c))
            return false;
        uint64_t at = lba(f, c) + (pos % cb) / 512;
        unsigned off = pos % 512, n = 512 - off;
        if (n > size - pos)
            n = size - pos;
        if (off || n < 512) {
            if (!rd(f, at, 1, f->work))
                return false;
        }
        memset(f->work + off, 0, n);
        if (!wr(f, at, 1, f->work))
            return false;
        pos += n;
    }
    if (!fat32_flush(f))
        return false;
    grown.size = size;
    grown.attributes |= 0x20;
    if (!publish(f, file, &grown))
        return false;
    *file = grown;
    if (allocating && (!info_write(f, f->free_count, f->next_free) || !fat32_flush(f)))
        return false;
    return true;
}
static bool make_sfn(const char *name, uint8_t *out)
{
    if (!name_valid(name))
        return false;
    memset(out, ' ', 11);
    unsigned base = 0, ext = 0;
    bool dot = false;
    for (unsigned i = 0; name[i]; ++i) {
        unsigned c = upper((uint8_t)name[i]);
        if (c == '.') {
            if (dot || !base)
                return false;
            dot = true;
            continue;
        }
        if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']')
            return false;
        if (dot) {
            if (ext == 3)
                return false;
            out[8 + ext++] = c;
        } else {
            if (base == 8)
                return false;
            out[base++] = c;
        }
    }
    return base && (!dot || ext);
}
bool fat32_create(struct fat32 *f, const char *name, struct fat32_file *file)
{
    uint8_t short_name[11];
    if (f->failed || !make_sfn(name, short_name))
        return false;
    struct fat32_file existing;
    if (lookup(f, name, &existing, true) || f->failed)
        return false;
    uint32_t c = f->root_cluster;
    uint64_t free_lba = 0;
    unsigned free_off = 0;
    for (uint32_t visited = 0; visited < f->clusters; ++visited) {
        for (unsigned s = 0; s < f->sectors_per_cluster; ++s) {
            uint64_t at = lba(f, c) + s;
            if (!rd(f, at, 1, f->work))
                return false;
            for (unsigned off = 0; off < 512; off += 32) {
                uint8_t *e = f->work + off;
                if (!e[0] || e[0] == 0xe5) {
                    free_lba = at;
                    free_off = off;
                    goto found;
                }
            }
        }
        uint32_t next;
        if (!fat_get(f, c, &next))
            return false;
        if (next >= EOC) {
            if (!info_write(f, UNKNOWN, UNKNOWN) || !fat32_flush(f) || !allocate(f, &next) ||
                !fat_set(f, c, next) || !fat32_flush(f))
                return bad(f);
            f->root_tail = next;
            free_lba = lba(f, next);
            free_off = 0;
            goto found;
        }
        if (!cluster(f, next))
            return bad(f);
        c = next;
    }
    return bad(f);
found:
    if (!rd(f, free_lba, 1, f->work))
        return false;
    uint8_t *e = f->work + free_off;
    bool end = !e[0];
    /* Preserve the end marker inside this sector. A root freshly extended
     * above is already zero. Crossing an existing end marker is handled by
     * clearing the following entry before making this entry visible. */
    if (end && free_off == 480) {
        uint64_t after = 0;
        unsigned s = (unsigned)(free_lba - lba(f, c));
        if (s + 1 < f->sectors_per_cluster)
            after = free_lba + 1;
        else {
            uint32_t next;
            if (!fat_get(f, c, &next))
                return false;
            if (next < EOC) {
                if (!cluster(f, next))
                    return bad(f);
                after = lba(f, next);
            }
        }
        if (after) {
            if (!rd(f, after, 1, f->work))
                return false;
            f->work[0] = 0;
            if (!wr(f, after, 1, f->work) || !fat32_flush(f))
                return false;
        }
        if (!rd(f, free_lba, 1, f->work))
            return false;
        e = f->work + free_off;
    }
    if (end && free_off < 480)
        e[32] = 0;
    memset(e, 0, 32);
    memcpy(e, short_name, 11);
    e[11] = 0x20;
    if (!wr(f, free_lba, 1, f->work) || !fat32_flush(f))
        return false;
    memset(file, 0, sizeof(*file));
    file->dir_lba = free_lba;
    file->dir_offset = free_off;
    file->attributes = 0x20;
    return true;
}
