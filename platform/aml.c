/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Acpi/Aml/{Parser,Namespace,Evaluator,Objects,Facts,Resources,Operators}.cpp
 */
/* C11 routing subset; ACPI 6.5 §§5.3, 6.4, 19, 20. Unsupported evaluation fails explicitly;
 * namespace scanning skips known declaration bodies, never byte-searches
 * for _PRT. Depth, object storage and execution operations are bounded.
 * Field units read PCI_Config regions only; other regions and all field
 * writes fail. Objects a method creates are deleted when it returns. */
#include "aml.h"
#include "ia64vmm/string.h"
#include "aml-grammar.h"

struct cursor {
    const uint8_t *p, *end;
};
struct frame {
    unsigned scope, slots[15], created;
    bool returned;
    unsigned result;
};
static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static bool take(struct cursor *c, unsigned n, uint64_t *v)
{
    if ((size_t)(c->end - c->p) < n || n > 8)
        return false;
    *v = le(c->p, n);
    c->p += n;
    return true;
}
/* §20.2.4 PkgLength. */
static bool length_value(struct cursor *c, uint64_t *length)
{
    uint64_t lead, tail;
    if (!take(c, 1, &lead))
        return false;
    unsigned following = lead >> 6;
    if ((following && (lead & 0x30)) || !take(c, following, &tail))
        return false;
    *length = (lead & (following ? 15 : 63)) | (tail << 4);
    return true;
}
static bool package(struct cursor *c, const uint8_t **end)
{
    const uint8_t *start = c->p;
    uint64_t length;
    if (!length_value(c, &length))
        return false;
    if (length < (uint64_t)(c->p - start) || length > (uint64_t)(c->end - start))
        return false;
    *end = start + length;
    return true;
}
static bool lead(unsigned c)
{
    return c == '_' || (c >= 'A' && c <= 'Z') || c == '\\' || c == '^' || c == '.' || c == '/';
}
static bool segment(const uint8_t *p)
{
    for (unsigned i = 0; i < 4; ++i)
        if (!(p[i] == '_' || (p[i] >= 'A' && p[i] <= 'Z') || (i && p[i] >= '0' && p[i] <= '9')))
            return false;
    return true;
}
static unsigned child(struct aml_context *a, unsigned parent, unsigned segment, bool create)
{
    for (unsigned i = 2; i < a->nnodes; ++i)
        if (a->nodes[i].parent == parent && a->nodes[i].segment == segment)
            return i;
    if (!create || !parent || a->nnodes == AML_NODES)
        return 0;
    unsigned i = a->nnodes++;
    a->nodes[i].parent = parent;
    a->nodes[i].segment = segment;
    return i;
}
unsigned aml_child(struct aml_context *a, unsigned parent, const char name[4])
{
    return child(a, parent, le((const uint8_t *)name, 4), false);
}
static bool name(struct aml_context *a, struct cursor *c, unsigned scope, bool create,
                 unsigned *out)
{
    if (c->p == c->end)
        return false;
    bool explicit = false;
    if (*c->p == '\\') {
        ++c->p;
        scope = 1;
        explicit = true;
    }
    while (c->p != c->end && *c->p == '^') {
        ++c->p;
        explicit = true;
        if (!scope)
            return false;
        scope = a->nodes[scope].parent;
    }
    if (!scope || c->p == c->end)
        return false;
    unsigned count = 1;
    if (*c->p == 0) {
        ++c->p;
        *out = scope;
        return true;
    }
    if (*c->p == '.') {
        ++c->p;
        count = 2;
    } else if (*c->p == '/') {
        ++c->p;
        if (c->p == c->end || !(count = *c->p++))
            return false;
    }
    if ((size_t)(c->end - c->p) < count * 4u)
        return false;
    for (unsigned i = 0; i < count; ++i)
        if (!segment(c->p + 4 * i))
            return false;
    if (!create && !explicit && count == 1) {
        unsigned found = 0;
        while (scope && !(found = child(a, scope, le(c->p, 4), false)))
            scope = a->nodes[scope].parent;
        c->p += 4;
        *out = found;
        return true;
    }
    for (unsigned i = 0; i < count; ++i) {
        scope = child(a, scope, le(c->p, 4), create);
        c->p += 4;
    }
    *out = scope;
    return !create || scope != 0;
}
static bool packaged(unsigned op)
{
    return (op >= 0x10 && op <= 0x14) || (op >= 0xa0 && op <= 0xa2) ||
           (op >= 0x5b81 && op <= 0x5b87);
}
static bool skip(struct aml_context *a, struct cursor *c, unsigned scope, unsigned depth)
{
    if (depth > 32 || ++a->operations > 1000000 || c->p == c->end)
        return false;
    unsigned op = *c->p++;
    if (lead(op)) {
        --c->p;
        unsigned node;
        if (!name(a, c, scope, false, &node))
            return false;
        if (node && a->nodes[node].opcode == 0x14)
            for (unsigned i = 0; i < (a->nodes[node].flags & 7); ++i)
                if (!skip(a, c, scope, depth + 1))
                    return false;
        return true;
    }
    if (op == 0 || op == 1 || op == 255 || (op >= 0x60 && op <= 0x6e))
        return true;
    uint64_t ignored;
    if (op == 0x0a || op == 0x0b || op == 0x0c || op == 0x0e)
        return take(c, op == 0x0e ? 8 : 1u << (op - 0x0a), &ignored);
    if (op == 0x0d) {
        while (c->p != c->end && *c->p)
            ++c->p;
        if (c->p == c->end)
            return false;
        ++c->p;
        return true;
    }
    if (op == 0x5b) {
        if (c->p == c->end)
            return false;
        op = 0x5b00 | *c->p++;
    }
    if (packaged(op)) {
        const uint8_t *end;
        if (!package(c, &end))
            return false;
        c->p = end;
        return true;
    }
    const char *shape = aml_shape(op);
    if (!shape)
        return false;
    while (*shape) {
        char s = *shape++;
        if (s == 'n') {
            unsigned node;
            if (!name(a, c, scope, false, &node))
                return false;
        } else if (s == 'b' || s == 'w' || s == 'd') {
            if (!take(c, s == 'b' ? 1 : s == 'w' ? 2 : 4, &ignored))
                return false;
        } else if (s == 'e' || s == 't' || s == 'q') {
            if (!skip(a, c, scope, depth + 1))
                return false;
        } else
            return false;
    }
    return true;
}
/* §19.6.48 FieldList: NamedField, ReservedField and AccessField entries
 * define field units; another entry ends the list, leaving later names
 * undefined. A field of an unknown region defines nothing. */
static bool fields(struct aml_context *a, struct cursor c, unsigned scope)
{
    unsigned region;
    uint64_t flags, bit = 0, n;
    if (!name(a, &c, scope, false, &region) || !take(&c, 1, &flags))
        return false;
    if (!region || a->nodes[region].opcode != 0x5b80)
        return true;
    unsigned access = flags & 15;
    while (c.p < c.end) {
        if (*c.p == 0) {
            ++c.p;
            if (!length_value(&c, &n))
                return false;
            bit += n;
        } else if (*c.p == 1) {
            uint64_t type, attribute;
            ++c.p;
            if (!take(&c, 1, &type) || !take(&c, 1, &attribute))
                return false;
            access = type & 15;
        } else {
            if (c.end - c.p < 4 || !segment(c.p))
                return true;
            uint64_t seg = le(c.p, 4);
            c.p += 4;
            if (!length_value(&c, &n))
                return false;
            unsigned node = child(a, scope, (unsigned)seg, true);
            if (!node || a->nodes[node].opcode)
                return false;
            struct aml_node *f = &a->nodes[node];
            f->opcode = 0x5b81;
            f->flags = access;
            f->target = region;
            f->integer32 = a->load32;
            f->bit = (unsigned)bit;
            f->width = (unsigned)n;
            bit += n;
        }
        if (bit > UINT32_MAX)
            return false;
    }
    return true;
}
static bool declarations(struct aml_context *a, struct cursor c, unsigned scope, unsigned depth)
{
    if (depth > 32)
        return false;
    while (c.p < c.end) {
        if (++a->operations > 1000000)
            return false;
        struct cursor start = c;
        unsigned op = *c.p++;
        if (op == 0x5b) {
            if (c.p == c.end)
                return false;
            op = 0x5b00 | *c.p++;
        }
        if (op == 0x08 || op == 0x14 || op == 0x10 || (op >= 0x5b82 && op <= 0x5b85)) {
            const uint8_t *end = c.end;
            if (op != 0x08 && !package(&c, &end))
                return false;
            struct cursor body = {c.p, end};
            unsigned node;
            if (!name(a, &body, scope, true, &node) || !node)
                return false;
            struct aml_node *n = &a->nodes[node];
            if (op == 0x10) {
                if (!declarations(a, body, node, depth + 1))
                    return false;
            } else {
                uint64_t flags = 0;
                if (op == 0x14 && !take(&body, 1, &flags))
                    return false;
                if (op == 0x5b83 && !take(&body, 6, &flags))
                    return false;
                if (op == 0x5b84 && !take(&body, 3, &flags))
                    return false;
                if (n->opcode)
                    return false; /* Conflicting definitions are never inferred. */
                n->opcode = op;
                n->flags = flags;
                n->begin = body.p;
                n->integer32 = a->load32;
                if (op == 0x08) {
                    if (!skip(a, &body, scope, 0))
                        return false;
                    n->end = body.p;
                    c.p = body.p;
                    continue;
                }
                n->end = end;
                if (op != 0x14 && !declarations(a, body, node, depth + 1))
                    return false;
            }
            c.p = end;
        } else if (op == 0x5b80) {
            /* §19.6.100 OperationRegion: space, then offset and length TermArgs. */
            unsigned node;
            uint64_t space;
            if (!name(a, &c, scope, true, &node) || !node || !take(&c, 1, &space))
                return false;
            struct aml_node *n = &a->nodes[node];
            if (n->opcode)
                return false;
            n->opcode = op;
            n->flags = space;
            n->begin = c.p;
            n->integer32 = a->load32;
            if (!skip(a, &c, scope, 0) || !skip(a, &c, scope, 0))
                return false;
            n->end = c.p;
        } else if (op == 0x5b81) {
            const uint8_t *end;
            if (!package(&c, &end) || !fields(a, (struct cursor){c.p, end}, scope))
                return false;
            c.p = end;
        } else {
            c = start;
            if (!skip(a, &c, scope, 0))
                return false;
        }
    }
    return true;
}
void aml_init(struct aml_context *a)
{
    memset(a, 0, sizeof(*a));
    a->nnodes = 2;
    a->nobjects = 1;
    a->integer_mask = UINT64_MAX;
    static const char scopes[][5] = {"_SB_", "_PR_", "_TZ_", "_SI_", "_GPE"};
    for (unsigned i = 0; i < 5; ++i)
        child(a, 1, le((const uint8_t *)scopes[i], 4), true);
}
bool aml_load(struct aml_context *a, const uint8_t *table, size_t size)
{
    if (!table || size < 36 || size > 1048576 || le(table + 4, 4) != size ||
        (memcmp(table, "DSDT", 4) && memcmp(table, "SSDT", 4)))
        return false;
    uint8_t sum = 0;
    for (size_t i = 0; i < size; ++i)
        sum += table[i];
    if (sum)
        return false;
    a->operations = 0;
    a->load32 = table[8] == 1;
    return declarations(a, (struct cursor){table + 36, table + size}, 1, 0);
}
static unsigned object(struct aml_context *a, struct aml_object value)
{
    if (a->nobjects == AML_OBJECTS) {
        a->error = 1;
        return 0;
    }
    a->objects[a->nobjects] = value;
    return a->nobjects++;
}
unsigned aml_integer(struct aml_context *a, uint64_t v)
{
    return object(a, (struct aml_object){.kind = AML_INTEGER, .integer = v & a->integer_mask});
}
const struct aml_object *aml_object(struct aml_context *a, unsigned id)
{
    return &a->objects[id < a->nobjects ? id : 0];
}
static unsigned eval(struct aml_context *, struct cursor *, struct frame *, unsigned);
static bool terms(struct aml_context *, struct cursor, struct frame *, unsigned);
/* §19.6.17-21, §19.6.24: CreateBit/Byte/Word/DWord/QWordField widths. */
static unsigned buffer_field(unsigned op)
{
    return op == 0x8d   ? 1
           : op == 0x8c ? 8
           : op == 0x8b ? 16
           : op == 0x8a ? 32
           : op == 0x8f ? 64
                        : 0;
}
static unsigned bits(const struct aml_node *n)
{
    return n->width && n->width <= (n->integer32 ? 32u : 64u) ? n->width : 0;
}
static uint64_t mask(unsigned width)
{
    return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}
/* §19.6.100 PCI_Config, §19.6.48 Field: naturally aligned reads of the
 * access width (AnyAcc and ByteAcc read bytes) covering the field unit. */
static unsigned field_read(struct aml_context *a, const struct aml_node *n, unsigned depth)
{
    static const unsigned sizes[] = {1, 1, 2, 4};
    const struct aml_node *r = &a->nodes[n->target];
    unsigned width = bits(n);
    if (r->opcode != 0x5b80 || r->flags != 2 || !r->target || !a->config_read || n->flags > 3 ||
        !width)
        goto fail;
    struct frame f = {.scope = r->parent};
    struct cursor c = {r->begin, r->end};
    unsigned offset = eval(a, &c, &f, depth + 1), length = eval(a, &c, &f, depth + 1);
    if (a->error || aml_object(a, offset)->kind != AML_INTEGER ||
        aml_object(a, length)->kind != AML_INTEGER)
        goto fail;
    uint64_t base = a->objects[offset].integer, size = a->objects[length].integer,
             unit = sizes[n->flags];
    uint64_t first = n->bit / 8 / unit * unit,
             last = (n->bit + (uint64_t)width - 1) / 8 / unit * unit;
    if (size > 4096 || base > 4096 - size || (n->bit + (uint64_t)width + 7) / 8 > size ||
        (base + first) % unit)
        goto fail;
    uint64_t value = 0;
    for (uint64_t at = first; at <= last; at += unit) {
        uint64_t part = a->config_read(a->config_context, (uint16_t)(r->target - 1),
                                       (unsigned)(base + at), (unsigned)unit);
        value |= at * 8 >= n->bit ? part << (at * 8 - n->bit) : part >> (n->bit - at * 8);
    }
    return aml_integer(a, value & mask(width));
fail:
    a->error = 1;
    return 0;
}
static uint8_t *writable(struct aml_context *a, struct aml_object *b)
{
    uintptr_t at = (uintptr_t)b->bytes, heap = (uintptr_t)a->heap;
    if (at >= heap && at - heap < AML_HEAP)
        return a->heap + (at - heap);
    if (b->length > AML_HEAP - a->nheap)
        return NULL;
    uint8_t *copy = a->heap + a->nheap;
    memcpy(copy, b->bytes, b->length);
    a->nheap += b->length;
    b->bytes = copy;
    return copy;
}
static unsigned evaluate(struct aml_context *a, unsigned id, const unsigned *args, unsigned count,
                         unsigned depth)
{
    if (!id || id >= a->nnodes || depth > 32) {
        a->error = 1;
        return 0;
    }
    struct aml_node *n = &a->nodes[id];
    if (n->busy) {
        a->error = 1;
        return 0;
    }
    if (n->opcode == 0x08 && n->value)
        return n->value;
    if (n->opcode == 0x5b81 && !count)
        return field_read(a, n, depth);
    if (buffer_field(n->opcode) && !count) {
        const struct aml_object *b = aml_object(a, n->target);
        unsigned width = bits(n);
        if (b->kind != AML_BUFFER || !width) {
            a->error = 1;
            return 0;
        }
        uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= (uint64_t)(b->bytes[(n->bit + i) / 8] >> ((n->bit + i) % 8) & 1) << i;
        return aml_integer(a, value);
    }
    if (n->opcode != 0x08 && n->opcode != 0x14) {
        a->error = 1;
        return 0;
    }
    if (count != (n->opcode == 0x14 ? (n->flags & 7) : 0)) {
        a->error = 1;
        return 0;
    }
    /* A method is the scope of its body (§5.3); a Name's data is evaluated
     * in its enclosing scope. */
    struct frame f = {.scope = n->opcode == 0x14 ? id : n->parent, .created = a->nnodes};
    for (unsigned i = 0; i < count; ++i)
        f.slots[i + 8] = args[i];
    struct cursor c = {n->begin, n->end};
    uint64_t saved = a->integer_mask;
    a->integer_mask = n->integer32 ? UINT32_MAX : UINT64_MAX;
    n->busy = true;
    unsigned v;
    if (n->opcode == 0x08) {
        v = eval(a, &c, &f, depth + 1);
        if (!a->error)
            n->value = v;
    } else {
        terms(a, c, &f, depth + 1);
        v = f.result;
        while (a->nnodes > f.created)
            memset(&a->nodes[--a->nnodes], 0, sizeof(a->nodes[0]));
    }
    n->busy = false;
    a->integer_mask = saved;
    return a->error ? 0 : v;
}
static bool store(struct aml_context *a, struct cursor *c, struct frame *f, unsigned v)
{
    if (c->p == c->end)
        return false;
    unsigned op = *c->p;
    if (op == 0) {
        ++c->p;
        return true;
    }
    if (op >= 0x60 && op <= 0x6e) {
        ++c->p;
        f->slots[op - 0x60] = v;
        return true;
    }
    unsigned node;
    if (!name(a, c, f->scope, false, &node) || !node)
        return false;
    struct aml_node *n = &a->nodes[node];
    if (n->opcode == 8) {
        n->value = v;
        return true;
    }
    /* §19.3.5.7: an Integer stored to a buffer field fills it, zero-extended.
     * Only buffers this evaluation created and no earlier named object holds
     * are written, so evaluation leaves the namespace unchanged. */
    const struct aml_object *source = aml_object(a, v);
    if (!buffer_field(n->opcode) || source->kind != AML_INTEGER ||
        n->target < a->evaluation_objects || n->target >= a->nobjects)
        return false;
    for (unsigned i = 2; i < a->evaluation_nodes; ++i)
        if (a->nodes[i].value == n->target)
            return false;
    struct aml_object *b = &a->objects[n->target];
    uint8_t *bytes = b->kind == AML_BUFFER ? writable(a, b) : NULL;
    if (!bytes)
        return false;
    for (unsigned i = 0; i < n->width; ++i) {
        unsigned at = n->bit + i;
        bytes[at / 8] = (uint8_t)((bytes[at / 8] & ~(1u << at % 8)) |
                                  (i < 64 && (source->integer >> i & 1)) << at % 8);
    }
    return true;
}
static unsigned eval(struct aml_context *a, struct cursor *c, struct frame *f, unsigned depth)
{
    if (a->error || depth > 32 || ++a->operations > 100000 || c->p == c->end)
        goto fail;
    unsigned op = *c->p++;
    if (op == 0 || op == 1 || op == 255)
        return aml_integer(a, op == 255 ? UINT64_MAX : op);
    uint64_t value;
    if (op == 0x0a || op == 0x0b || op == 0x0c || op == 0x0e) {
        if (!take(c, op == 0x0e ? 8 : 1u << (op - 0x0a), &value))
            goto fail;
        return aml_integer(a, value);
    }
    if (op >= 0x60 && op <= 0x6e) {
        unsigned id = f->slots[op - 0x60];
        if (!id)
            goto fail;
        return id;
    }
    if (lead(op)) {
        --c->p;
        unsigned node;
        if (!name(a, c, f->scope, false, &node) || !node)
            goto fail;
        unsigned args[7], n = a->nodes[node].opcode == 0x14 ? a->nodes[node].flags & 7 : 0;
        for (unsigned i = 0; i < n; ++i)
            args[i] = eval(a, c, f, depth + 1);
        return evaluate(a, node, args, n, depth + 1);
    }
    if (op == 0x0d) {
        const uint8_t *start = c->p;
        while (c->p < c->end && *c->p)
            ++c->p;
        if (c->p == c->end)
            goto fail;
        unsigned n = c->p++ - start;
        return object(a, (struct aml_object){.kind = AML_STRING, .bytes = start, .length = n});
    }
    if (op == 0x11 || op == 0x12 || op == 0x13) {
        const uint8_t *end;
        if (!package(c, &end))
            goto fail;
        struct cursor body = {c->p, end};
        uint64_t n;
        if (op == 0x12) {
            if (!take(&body, 1, &n))
                goto fail;
        } else {
            unsigned id = eval(a, &body, f, depth + 1);
            if (aml_object(a, id)->kind != AML_INTEGER)
                goto fail;
            n = a->objects[id].integer;
        }
        if (op == 0x11) {
            if (n != (uint64_t)(end - body.p))
                goto fail;
            c->p = end;
            return object(a, (struct aml_object){.kind = AML_BUFFER, .bytes = body.p, .length = n});
        }
        if (n > 4096 || n > AML_ELEMENTS - a->nelements)
            goto fail;
        unsigned first = a->nelements;
        a->nelements += n;
        for (unsigned i = 0; i < n; ++i) {
            if (body.p == body.end)
                goto fail;
            /* Package NameStrings are references (ACPI §20 DataRefObject),
             * including link devices which cannot be evaluated as integers. */
            if (lead(*body.p)) {
                unsigned node;
                if (!name(a, &body, f->scope, false, &node) || !node)
                    goto fail;
                a->elements[first + i] =
                    object(a, (struct aml_object){.kind = AML_REFERENCE, .first = node});
            } else
                a->elements[first + i] = eval(a, &body, f, depth + 1);
        }
        if (body.p != end)
            goto fail;
        c->p = end;
        return object(a, (struct aml_object){.kind = AML_PACKAGE, .first = first, .length = n});
    }
    if (op == 0x70) {
        unsigned id = eval(a, c, f, depth + 1);
        if (!store(a, c, f, id))
            goto fail;
        return id;
    }
    if (op == 0x92) {
        unsigned id = eval(a, c, f, depth + 1);
        if (aml_object(a, id)->kind != AML_INTEGER)
            goto fail;
        return aml_integer(a, a->objects[id].integer ? 0 : UINT64_MAX);
    }
    if (op == 0x72 || op == 0x74 || op == 0x77 || op == 0x79 || op == 0x7a || op == 0x7b ||
        op == 0x7d || op == 0x7f || (op >= 0x90 && op <= 0x95 && op != 0x92)) {
        unsigned left = eval(a, c, f, depth + 1), right = eval(a, c, f, depth + 1);
        if (aml_object(a, left)->kind != AML_INTEGER || aml_object(a, right)->kind != AML_INTEGER)
            goto fail;
        uint64_t x = a->objects[left].integer, y = a->objects[right].integer;
        switch (op) {
        case 0x72:
            value = x + y;
            break;
        case 0x74:
            value = x - y;
            break;
        case 0x77:
            value = x * y;
            break;
        case 0x79:
            value = y >= 64 ? 0 : x << y;
            break;
        case 0x7a:
            value = y >= 64 ? 0 : x >> y;
            break;
        case 0x7b:
            value = x & y;
            break;
        case 0x7d:
            value = x | y;
            break;
        case 0x7f:
            value = x ^ y;
            break;
        case 0x90:
            value = x && y;
            break;
        case 0x91:
            value = x || y;
            break;
        case 0x93:
            value = x == y;
            break;
        case 0x94:
            value = x > y;
            break;
        default:
            value = x < y;
            break;
        }
        /* ACPI §19 logical operators return Ones for true;
         * subsequent integer arithmetic can observe it. */
        if (op >= 0x90)
            value = value ? UINT64_MAX : 0;
        unsigned result = aml_integer(a, value);
        if (op < 0x90 && !store(a, c, f, result))
            goto fail;
        return result;
    }
fail:
    a->error = 1;
    return 0;
}
static bool terms(struct aml_context *a, struct cursor c, struct frame *f, unsigned depth)
{
    if (depth > 32) {
        a->error = 1;
        return false;
    }
    while (c.p < c.end && !f->returned && !a->error) {
        unsigned op = *c.p;
        if (op == 0xa4) {
            ++c.p;
            f->result = eval(a, &c, f, depth + 1);
            f->returned = true;
        } else if (op == 0xa0) {
            ++c.p;
            const uint8_t *end;
            if (!package(&c, &end))
                goto fail;
            struct cursor branch = {c.p, end};
            unsigned predicate = eval(a, &branch, f, depth + 1);
            if (aml_object(a, predicate)->kind != AML_INTEGER)
                goto fail;
            bool taken = a->objects[predicate].integer != 0;
            if (taken && !terms(a, branch, f, depth + 1))
                return false;
            c.p = end;
            if (c.p < c.end && *c.p == 0xa1) {
                ++c.p;
                if (!package(&c, &end))
                    goto fail;
                if (!taken && !terms(a, (struct cursor){c.p, end}, f, depth + 1))
                    return false;
                c.p = end;
            }
        } else if (op == 0x08 || buffer_field(op)) {
            /* §19.6.90 Name, §19.6.17-24 CreateXField: new objects of this
             * invocation, named in the method's scope. */
            ++c.p;
            unsigned value, index = 0, node = 0;
            if (op == 0x08) {
                if (!name(a, &c, f->scope, true, &node))
                    goto fail;
                value = eval(a, &c, f, depth + 1);
            } else {
                value = eval(a, &c, f, depth + 1);
                index = eval(a, &c, f, depth + 1);
                if (!a->error && !name(a, &c, f->scope, true, &node))
                    goto fail;
            }
            if (a->error || node < f->created || a->nodes[node].opcode)
                goto fail;
            struct aml_node *n = &a->nodes[node];
            n->opcode = op;
            n->integer32 = a->integer_mask == UINT32_MAX;
            if (op == 0x08) {
                n->value = value;
                continue;
            }
            const struct aml_object *b = aml_object(a, value), *i = aml_object(a, index);
            unsigned width = buffer_field(op);
            if (b->kind != AML_BUFFER || i->kind != AML_INTEGER || i->integer > UINT32_MAX / 8 ||
                i->integer * (op == 0x8d ? 1 : 8) + width > (uint64_t)b->length * 8)
                goto fail;
            n->target = value;
            n->bit = (unsigned)(i->integer * (op == 0x8d ? 1 : 8));
            n->width = width;
        } else if (op == 0xa3)
            ++c.p;
        else
            eval(a, &c, f, depth + 1);
    }
    return !a->error;
fail:
    a->error = 1;
    return false;
}
unsigned aml_evaluate(struct aml_context *a, unsigned node, const unsigned *args, unsigned count)
{
    a->operations = a->error = 0;
    if (count > 7 || (count && !args)) {
        a->error = 1;
        return 0;
    }
    unsigned objects = a->nobjects, elements = a->nelements, heap = a->nheap;
    a->evaluation_nodes = a->nnodes;
    a->evaluation_objects = objects;
    for (unsigned i = 0; i < a->nnodes; ++i)
        a->journal[i] = a->nodes[i].value;
    unsigned result = evaluate(a, node, args, count, 0);
    if (a->error) {
        /* Failed pure evaluation must not leave partial _PIC stores or lazy
         * caches that could affect later route decisions (rollback journal). */
        for (unsigned i = 0; i < a->nnodes; ++i)
            a->nodes[i].value = a->journal[i];
        a->nobjects = objects;
        a->nelements = elements;
        a->nheap = heap;
    }
    return result;
}
bool aml_irq_resource(const struct aml_object *o, unsigned index, uint32_t *gsi, bool *low,
                      bool *level)
{
    if (o->kind != AML_BUFFER)
        return false;
    unsigned at = 0, ordinal = 0;
    while (at < o->length) {
        unsigned tag = o->bytes[at++], n;
        if (tag & 128) {
            if (o->length - at < 2)
                return false;
            n = le(o->bytes + at, 2);
            at += 2;
        } else
            n = tag & 7;
        if (n > o->length - at)
            return false;
        const uint8_t *p = o->bytes + at;
        if (ordinal == index) {
            if ((tag >> 3) == 4 && (n == 2 || n == 3)) {
                unsigned mask = le(p, 2);
                if (!mask || (mask & (mask - 1)))
                    return false;
                *gsi = __builtin_ctz(mask);
                *level = n == 3 && !(p[2] & 1);
                *low = n == 3 && (p[2] & 8);
                return true;
            }
            if (tag == 0x89 && n >= 6 && p[1] == 1) {
                *gsi = le(p + 2, 4);
                *level = !(p[0] & 2);
                *low = p[0] & 4;
                return true;
            }
            return false;
        }
        if ((tag >> 3) == 15)
            break;
        ++ordinal;
        at += n;
    }
    return false;
}
