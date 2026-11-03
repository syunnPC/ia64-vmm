/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Ported from qemu-system-ia64, commit 5f67e29, hw/ia64/ia64_firmware_elf.c.
 * Replace GLib allocation and output with HAL scratch pages and coherent
 * gm_write copies. Also accept unrelaxed GOT data references in larger images. */
#include "platform/platform.h"
#include "platform/elf.h"
#include "ia64vmm/hal.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef struct {
    uint64_t offset, size;
} IA64FirmwareSegment;
typedef struct {
    uint64_t base, size, alignment, entry, global_pointer, pal_entry, pale_init, pale_check;
    uint8_t *data;
    IA64FirmwareSegment segments[64];
    unsigned segment_count;
} IA64FirmwareElf;
/* Scratch objects have one aligned allocation header so all error paths can
 * release them without depending on an OS allocator. */
static void *fw_alloc(size_t bytes)
{
    if (bytes > SIZE_MAX - 4111)
        return NULL;
    size_t pages = (bytes + 16 + 4095) / 4096;
    uint64_t *p = hal_alloc_pages(pages, HAL_PAGE_ZERO);
    if (!p)
        return NULL;
    p[0] = pages;
    return p + 2;
}
static void fw_free(void *p)
{
    if (p) {
        uint64_t *header = (uint64_t *)p - 2;
        hal_free_pages(header, header[0]);
    }
}
static uint16_t lduw_le_p(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}
static uint32_t ldl_le_p(const uint8_t *p)
{
    return lduw_le_p(p) | (uint32_t)lduw_le_p(p + 2) << 16;
}
static uint64_t ldq_le_p(const uint8_t *p)
{
    return ldl_le_p(p) | (uint64_t)ldl_le_p(p + 4) << 32;
}
static void stq_le_p(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i)
        p[i] = v >> (i * 8);
}
static bool has_zero(const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n; ++i)
        if (!p[i])
            return true;
    return false;
}

#define FW_SHF_TLS 0x400
/* The firmware embeds the DXE core, architectural drivers and HII/console/BDS PE
 * images. Keep a bounded loader allocation while accepting that larger FV. */
#define FW_MAX_SPAN (32 * 1024 * 1024)
#define FW_SLOT_MASK ((UINT64_C(1) << 41) - 1)

typedef struct FirmwareElfReader {
    const uint8_t *file;
    size_t length;
    Elf64_Shdr *sections;
    unsigned section_count;
    const char *names;
    size_t names_size;
    IA64FirmwareElf *out;
    uint8_t *linked;
    /* Per-byte flags: relocated word/bundle (1), referenced GOT word (2),
     * paired addl/ld8.mov instruction (4). Only bit 1 suppresses writes. */
    uint8_t *fixed;
    uint64_t delta;
} FirmwareElfReader;

static bool file_range(FirmwareElfReader *r, uint64_t offset, uint64_t size)
{
    return offset <= r->length && size <= r->length - offset;
}

static bool memory_range(FirmwareElfReader *r, uint64_t addr, uint64_t size)
{
    unsigned i;

    if (addr < r->out->base) {
        return false;
    }
    addr -= r->out->base;
    for (i = 0; i < r->out->segment_count; i++) {
        IA64FirmwareSegment *s = &r->out->segments[i];
        if (addr >= s->offset && addr - s->offset <= s->size &&
            size <= s->size - (addr - s->offset)) {
            return true;
        }
    }
    return false;
}

static const char *section_name(FirmwareElfReader *r, Elf64_Shdr *s)
{
    return r->names + s->sh_name;
}

static Elf64_Shdr *find_section(FirmwareElfReader *r, const char *name)
{
    unsigned i;

    for (i = 0; i < r->section_count; i++) {
        if (!strcmp(section_name(r, &r->sections[i]), name)) {
            return &r->sections[i];
        }
    }
    return NULL;
}

static bool executable_address(FirmwareElfReader *r, uint64_t address)
{
    unsigned i;

    for (i = 0; i < r->section_count; i++) {
        Elf64_Shdr *s = &r->sections[i];

        if ((s->sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) == (SHF_ALLOC | SHF_EXECINSTR) &&
            address >= s->sh_addr && address - s->sh_addr < s->sh_size && !(address & 15)) {
            return true;
        }
    }
    return false;
}

static bool symbol_read(FirmwareElfReader *r, Elf64_Shdr *table, uint64_t index, Elf64_Sym *sym)
{
    const uint8_t *p;

    if (table->sh_type != SHT_SYMTAB || table->sh_entsize != sizeof(*sym) ||
        table->sh_size % sizeof(*sym) || index >= table->sh_size / sizeof(*sym)) {
        return false;
    }
    p = r->file + table->sh_offset + index * sizeof(*sym);
    sym->st_name = ldl_le_p(p);
    sym->st_info = p[4];
    sym->st_other = p[5];
    sym->st_shndx = lduw_le_p(p + 6);
    sym->st_value = ldq_le_p(p + 8);
    sym->st_size = ldq_le_p(p + 16);
    return true;
}

static bool find_symbol(FirmwareElfReader *r, const char *name, uint64_t *value)
{
    unsigned i;

    for (i = 0; i < r->section_count; i++) {
        Elf64_Shdr *s = &r->sections[i];
        Elf64_Shdr *strings;
        uint64_t j;

        if (s->sh_type != SHT_SYMTAB || s->sh_link >= r->section_count) {
            continue;
        }
        strings = &r->sections[s->sh_link];
        if (strings->sh_type != SHT_STRTAB) {
            return false;
        }
        for (j = 0; j < s->sh_size / sizeof(Elf64_Sym); j++) {
            Elf64_Sym sym;
            const char *n;

            if (!symbol_read(r, s, j, &sym) || sym.st_name >= strings->sh_size) {
                return false;
            }
            n = (const char *)r->file + strings->sh_offset + sym.st_name;
            if (!has_zero(n, strings->sh_size - sym.st_name)) {
                return false;
            }
            if (!strcmp(n, name)) {
                if (sym.st_shndx == SHN_UNDEF || !memory_range(r, sym.st_value, 0)) {
                    return false;
                }
                *value = sym.st_value;
                return true;
            }
        }
    }
    return false;
}

static bool fix_pointer(FirmwareElfReader *r, uint64_t address, bool allow_zero)
{
    uint64_t offset = address - r->out->base;
    uint64_t value;

    if (!memory_range(r, address, 8) || (address & 7)) {
        return false;
    }
    if (r->fixed[offset] & 1) {
        return allow_zero;
    }
    value = ldq_le_p(r->linked + offset);
    if (allow_zero && !value) {
        return true;
    }
    if (!memory_range(r, value, 0)) {
        return false;
    }
    stq_le_p(r->out->data + offset, value + r->delta);
    r->fixed[offset] |= 1;
    return true;
}

static bool fix_imm64(FirmwareElfReader *r, uint64_t address, uint64_t value, bool relative)
{
    uint64_t bundle = address & ~UINT64_C(15);
    uint64_t offset = bundle - r->out->base;
    uint8_t *p;
    uint64_t low, high, slot1, slot2, actual;

    if ((address & 15) != 1 || !memory_range(r, bundle, 16) || (r->fixed[offset] & 1)) {
        return false;
    }
    p = r->out->data + offset;
    low = ldq_le_p(r->linked + offset);
    high = ldq_le_p(r->linked + offset + 8);
    slot1 = ((low >> 46) | (high << 18)) & FW_SLOT_MASK;
    slot2 = high >> 23;
    actual = ((slot2 >> 13) & 0x7f) | ((slot2 >> 27) & 0x1ff) << 7 | ((slot2 >> 22) & 0x1f) << 16 |
             ((slot2 >> 21) & 1) << 21 | slot1 << 22 | ((slot2 >> 36) & 1) << 63;
    if ((low & 0x1e) != 4 || (slot2 >> 37) != 6 || actual != value) {
        return false;
    }
    if (relative) {
        return true;
    }
    value += r->delta;
    slot1 = (value >> 22) & FW_SLOT_MASK;
    slot2 &= ~((UINT64_C(1) << 36) | (UINT64_C(0x1ff) << 27) | (UINT64_C(0x1f) << 22) |
               (UINT64_C(1) << 21) | (UINT64_C(0x7f) << 13));
    slot2 |= ((value >> 63) & 1) << 36 | ((value >> 7) & 0x1ff) << 27 |
             ((value >> 16) & 0x1f) << 22 | ((value >> 21) & 1) << 21 | (value & 0x7f) << 13;
    stq_le_p(p, (low & ((UINT64_C(1) << 46) - 1)) | (slot1 << 46));
    stq_le_p(p + 8, (slot1 >> 18) | (slot2 << 23));
    r->fixed[offset] |= 1;
    return true;
}

static bool read_instruction(FirmwareElfReader *r, uint64_t address, char *unit_out,
                             uint64_t *insn_out)
{
    static const char units[16][4] = {
        "MII", "MII", "MLX", "---", "MMI", "MMI", "MFI", "MMF",
        "MIB", "MBB", "---", "BBB", "MMB", "---", "MFB", "---",
    };
    uint64_t bundle = address & ~UINT64_C(15);
    unsigned slot = address & 15;
    const uint8_t *p;
    uint64_t low, high, insn;
    char unit;

    if (slot > 2 || !memory_range(r, bundle, 16)) {
        return false;
    }
    p = r->linked + bundle - r->out->base;
    low = ldq_le_p(p);
    high = ldq_le_p(p + 8);
    insn = (slot == 0   ? low >> 5
            : slot == 1 ? (low >> 46) | (high << 18)
                        : high >> 23) &
           FW_SLOT_MASK;
    unit = units[(low & 31) >> 1][slot];
    *unit_out = unit;
    *insn_out = insn;
    return true;
}

/* Match one marker to one preceding addl in the same relocation section,
 * target and register chain. Consume instructions, not sets of symbols. */
static bool pair_ldxmov(FirmwareElfReader *r, Elf64_Shdr *s, Elf64_Shdr *symbols, uint64_t address,
                        uint64_t value)
{
    char unit;
    uint64_t marker, best = UINT64_MAX;
    if (!read_instruction(r, address, &unit, &marker) || (r->fixed[address - r->out->base] & 4))
        return false;
    for (uint64_t j = 0; j < s->sh_size; j += sizeof(Elf64_Rela)) {
        const uint8_t *p = r->file + s->sh_offset + j;
        uint64_t at = ldq_le_p(p), info = ldq_le_p(p + 8), insn;
        Elf64_Sym sym;
        if (ELF64_R_TYPE(info) != R_IA64_LTOFF22X || at >= address || !memory_range(r, at, 1) ||
            (r->fixed[at - r->out->base] & 4) ||
            !symbol_read(r, symbols, ELF64_R_SYM(info), &sym) ||
            sym.st_value + ldq_le_p(p + 16) != value || !read_instruction(r, at, &unit, &insn) ||
            ((insn >> 6) & 127) != ((marker >> 20) & 127))
            continue;
        if (best == UINT64_MAX || at > best)
            best = at;
    }
    if (best == UINT64_MAX)
        return false;
    r->fixed[best - r->out->base] |= 4;
    r->fixed[address - r->out->base] |= 4;
    return true;
}

static bool relative_instruction(FirmwareElfReader *r, uint64_t address, uint64_t value,
                                 uint32_t type, Elf64_Shdr *got, Elf64_Shdr *opd)
{
    uint64_t insn, actual;
    char unit;
    uint64_t bundle = address & ~UINT64_C(15);
    if (!read_instruction(r, address, &unit, &insn))
        return false;
    if (type == R_IA64_LDXMOV) {
        /* GNU ld's unrelaxed ld8.mov marker carries no address bits. */
        uint64_t operands = 63 | (UINT64_C(127) << 6) | (UINT64_C(127) << 20);
        if (unit == 'M' && (insn & ~operands) == UINT64_C(0x80c0000000))
            return true;
        LOGE("invalid LDXMOV instruction");
        return false;
    }
    if (type == R_IA64_PCREL21B) {
        actual = ((insn >> 13) & 0xfffff) | ((insn >> 36) & 1) << 20;
        actual = (uint64_t)((int64_t)(actual << 43) >> 43) << 4;
        return unit == 'B' && ((insn >> 37) == 4 || (insn >> 37) == 5) &&
               executable_address(r, value) && actual == value - bundle;
    }
    if ((unit != 'M' && unit != 'I') || (insn >> 37) != 9 || ((insn >> 20) & 3) != 1) {
        return false;
    }
    actual = ((insn >> 13) & 0x7f) | ((insn >> 27) & 0x1ff) << 7 | ((insn >> 22) & 0x1f) << 16 |
             ((insn >> 36) & 1) << 21;
    actual = (int64_t)(actual << 42) >> 42;
    if (type == R_IA64_GPREL22) {
        return actual == value - r->out->global_pointer;
    }
    actual += r->out->global_pointer;
    if (actual < got->sh_addr || actual - got->sh_addr >= got->sh_size || (actual & 7)) {
        return false;
    }
    r->fixed[actual - r->out->base] |= 2;
    actual = ldq_le_p(r->linked + actual - r->out->base);
    if (type == R_IA64_LTOFF22 || type == R_IA64_LTOFF22X) {
        if (actual == value)
            return true;
    } else {
        if (actual < opd->sh_addr || actual - opd->sh_addr >= opd->sh_size ||
            ((actual - opd->sh_addr) & 15) || (actual & 15)) {
            LOGE("invalid GOT function descriptor");
            return false;
        }
        if (ldq_le_p(r->linked + actual - r->out->base) == value)
            return true;
    }
    LOGE("wrong GOT slot");
    return false;
}

static bool relocate(FirmwareElfReader *r)
{
    Elf64_Shdr *opd = find_section(r, ".opd");
    Elf64_Shdr *got = find_section(r, ".got");
    unsigned i;
    unsigned relocations = 0;

    if (!opd || !got || !(opd->sh_flags & SHF_ALLOC) || !(got->sh_flags & SHF_ALLOC) ||
        (opd->sh_addr & 15) || (opd->sh_size & 15) || (got->sh_size & 7)) {
        goto invalid;
    }
    for (i = 0; i < opd->sh_size; i += 16) {
        const uint8_t *p = r->linked + opd->sh_addr - r->out->base + i;

        if (!executable_address(r, ldq_le_p(p)) || ldq_le_p(p + 8) != r->out->global_pointer) {
            goto invalid;
        }
    }
    for (i = 0; i < got->sh_size; i += 8) {
        uint64_t value = ldq_le_p(r->linked + got->sh_addr - r->out->base + i);

        /* ld also synthesizes data/end-symbol pointers for LTOFF22(X).
         * Each referring instruction validates its own slot below. */
        if (!memory_range(r, value, 0) ||
            (value >= opd->sh_addr && value - opd->sh_addr < opd->sh_size &&
             ((value - opd->sh_addr) & 15))) {
            LOGE("invalid GOT function descriptor");
            goto invalid;
        }
    }
    for (i = 0; i < r->section_count; i++) {
        Elf64_Shdr *s = &r->sections[i];
        Elf64_Shdr *target, *symbols;
        uint64_t j;

        if (s->sh_type != SHT_RELA) {
            if (s->sh_type == SHT_REL || s->sh_type == SHT_DYNAMIC) {
                goto invalid;
            }
            continue;
        }
        if (s->sh_info >= r->section_count || s->sh_link >= r->section_count ||
            s->sh_entsize != sizeof(Elf64_Rela) || s->sh_size % sizeof(Elf64_Rela)) {
            goto invalid;
        }
        target = &r->sections[s->sh_info];
        symbols = &r->sections[s->sh_link];
        if (!(target->sh_flags & SHF_ALLOC)) {
            continue;
        }
        for (j = 0; j < s->sh_size; j += sizeof(Elf64_Rela)) {
            const uint8_t *p = r->file + s->sh_offset + j;
            uint64_t address = ldq_le_p(p);
            uint64_t info = ldq_le_p(p + 8);
            uint64_t addend = ldq_le_p(p + 16);
            uint32_t type = ELF64_R_TYPE(info);
            Elf64_Sym sym;
            uint64_t value;
            bool ok;

            if (type == R_IA64_NONE) {
                continue;
            }
            if (!symbol_read(r, symbols, ELF64_R_SYM(info), &sym) || sym.st_shndx == SHN_UNDEF ||
                (sym.st_shndx != SHN_ABS && (sym.st_shndx >= r->section_count ||
                                             !(r->sections[sym.st_shndx].sh_flags & SHF_ALLOC))) ||
                address < target->sh_addr || address - target->sh_addr >= target->sh_size) {
                goto invalid;
            }
            value = sym.st_value + addend;
            if (!memory_range(r, value, 0)) {
                goto invalid;
            }
            relocations++;
            switch (type) {
            case R_IA64_IMM64:
                ok = fix_imm64(r, address, value, false);
                break;
            case R_IA64_DIR64LSB:
                if (target->sh_size - (address - target->sh_addr) < 8 ||
                    ldq_le_p(r->linked + address - r->out->base) != value) {
                    goto invalid;
                }
                ok = fix_pointer(r, address, false);
                break;
            case R_IA64_FPTR64LSB:
                if (target->sh_size - (address - target->sh_addr) < 8) {
                    goto invalid;
                }
                {
                    uint64_t descriptor = ldq_le_p(r->linked + address - r->out->base);

                    if (descriptor < opd->sh_addr || descriptor - opd->sh_addr >= opd->sh_size ||
                        ((descriptor - opd->sh_addr) & 15) ||
                        ldq_le_p(r->linked + descriptor - r->out->base) != value) {
                        goto invalid;
                    }
                }
                ok = fix_pointer(r, address, false);
                break;
            case R_IA64_GPREL22:
            case R_IA64_LTOFF22:
            case R_IA64_LTOFF22X:
            case R_IA64_LDXMOV:
            case R_IA64_LTOFF_FPTR22:
            case R_IA64_PCREL21B:
                ok = relative_instruction(r, address, value, type, got, opd);
                if (ok && type == R_IA64_LDXMOV && !pair_ldxmov(r, s, symbols, address, value)) {
                    LOGE("unpaired LTOFF22X/LDXMOV relocation");
                    ok = false;
                }
                break;
            case R_IA64_GPREL64I:
                ok = fix_imm64(r, address, value - r->out->global_pointer, true);
                break;
            case R_IA64_PCREL64LSB:
            case R_IA64_SEGREL64LSB:
                ok = !(address & 7) && memory_range(r, address, 8) &&
                     target->sh_size - (address - target->sh_addr) >= 8;
                if (type == R_IA64_PCREL64LSB) {
                    value -= address;
                } else {
                    unsigned k;

                    for (k = 0; k < r->out->segment_count; k++) {
                        IA64FirmwareSegment *seg = &r->out->segments[k];
                        uint64_t start = r->out->base + seg->offset;

                        if (target->sh_addr >= start && target->sh_addr - start < seg->size) {
                            value = value > start ? value - start : 0;
                            break;
                        }
                    }
                }
                ok = ok && ldq_le_p(r->linked + address - r->out->base) == value;
                break;
            default:
                LOGE("unsupported IA-64 firmware relocation 0x%x", type);
                return false;
            }
            if (!ok) {
                goto invalid;
            }
        }
    }
    for (i = 0; i < r->section_count; i++) {
        Elf64_Shdr *s = &r->sections[i];
        if (s->sh_type != SHT_RELA || !(r->sections[s->sh_info].sh_flags & SHF_ALLOC))
            continue;
        for (uint64_t j = 0; j < s->sh_size; j += sizeof(Elf64_Rela)) {
            const uint8_t *p = r->file + s->sh_offset + j;
            if (ELF64_R_TYPE(ldq_le_p(p + 8)) == R_IA64_LTOFF22X &&
                !(r->fixed[ldq_le_p(p) - r->out->base] & 4)) {
                LOGE("unpaired LTOFF22X/LDXMOV relocation");
                return false;
            }
        }
    }
    for (uint64_t j = 0; j < got->sh_size; j += 8) {
        uint64_t addr = got->sh_addr + j;
        uint64_t value = ldq_le_p(r->linked + addr - r->out->base);
        bool descriptor = value >= opd->sh_addr && value - opd->sh_addr < opd->sh_size &&
                          !((value - opd->sh_addr) & 15);
        if (!descriptor && !(r->fixed[addr - r->out->base] & 2)) {
            LOGE("unreferenced firmware GOT entry");
            return false;
        }
    }
    if (!relocations) {
        goto invalid;
    }
    /* GNU ld synthesizes these words without output relocation records. */
    for (i = 0; i < r->section_count; i++) {
        Elf64_Shdr *s = &r->sections[i];
        const char *name = section_name(r, s);
        uint64_t j;

        if (strcmp(name, ".opd") && strcmp(name, ".got")) {
            continue;
        }
        if (!(s->sh_flags & SHF_ALLOC) || (s->sh_addr & 7) ||
            (s->sh_size % (!strcmp(name, ".opd") ? 16 : 8))) {
            goto invalid;
        }
        for (j = 0; j < s->sh_size; j += 8) {
            if (!fix_pointer(r, s->sh_addr + j, true)) {
                goto invalid;
            }
        }
    }
    return true;

invalid:
    LOGE("invalid IA-64 firmware relocation or symbol");
    return false;
}

static void ia64_firmware_elf_clear(IA64FirmwareElf *image)
{
    fw_free(image->data);
    memset(image, 0, sizeof(*image));
}

static bool parse_elf(const void *image, size_t image_size, uint64_t load_base,
                      IA64FirmwareElf *out)
{
    FirmwareElfReader r = {.file = image, .length = image_size, .out = out};
    const uint8_t *h = image;
    uint64_t phoff, shoff, end = 0, payload_end;
    uint16_t phnum, shnum, names;
    unsigned i;
    bool success = false;

    memset(out, 0, sizeof(*out));
    if (!file_range(&r, 0, sizeof(Elf64_Ehdr)) || memcmp(h, ELFMAG, SELFMAG) ||
        h[EI_CLASS] != ELFCLASS64 || h[EI_DATA] != ELFDATA2LSB || h[EI_VERSION] != EV_CURRENT ||
        lduw_le_p(h + 16) != ET_EXEC || lduw_le_p(h + 18) != EM_IA_64 ||
        ldl_le_p(h + 20) != EV_CURRENT || lduw_le_p(h + 52) != sizeof(Elf64_Ehdr) ||
        lduw_le_p(h + 54) != sizeof(Elf64_Phdr) || lduw_le_p(h + 58) != sizeof(Elf64_Shdr)) {
        goto invalid;
    }
    phoff = ldq_le_p(h + 32);
    shoff = ldq_le_p(h + 40);
    phnum = lduw_le_p(h + 56);
    shnum = lduw_le_p(h + 60);
    names = lduw_le_p(h + 62);
    if (!phnum || !shnum || names >= shnum || !file_range(&r, phoff, phnum * sizeof(Elf64_Phdr)) ||
        !file_range(&r, shoff, shnum * sizeof(Elf64_Shdr))) {
        goto invalid;
    }
    out->base = UINT64_MAX;
    out->alignment = 0x2000;

    for (i = 0; i < phnum; i++) {
        const uint8_t *p = h + phoff + i * sizeof(Elf64_Phdr);
        uint64_t addr = ldq_le_p(p + 16), size = ldq_le_p(p + 40);
        uint64_t align = ldq_le_p(p + 48), offset = ldq_le_p(p + 8);
        uint64_t filesz = ldq_le_p(p + 32);

        if (ldl_le_p(p) != PT_LOAD) {
            if (ldl_le_p(p) == PT_DYNAMIC || ldl_le_p(p) == PT_INTERP) {
                goto invalid;
            }
            continue;
        }
        if (!size || filesz > size || !file_range(&r, offset, filesz) || addr > UINT64_MAX - size ||
            ldq_le_p(p + 24) != addr ||
            (align && ((align & (align - 1)) || (addr & (align - 1)) != (offset & (align - 1))))) {
            goto invalid;
        }
        out->base = MIN(out->base, addr);
        end = MAX(end, addr + size);
        out->alignment = MAX(out->alignment, align);
    }
    if (end <= out->base || end - out->base >= FW_MAX_SPAN || (out->base & (out->alignment - 1))) {
        goto invalid;
    }
    out->size = end - out->base;
    if (!load_base) {
        if (platform_ram_contains(out->base, out->size)) {
            load_base = out->base;
        } else if (!platform_find_ram(out->size, out->alignment, UINT64_MAX, NULL, 0, false,
                                      &load_base)) {
            goto invalid;
        }
    }
    if (!platform_ram_contains(load_base, out->size))
        goto invalid;
    if ((load_base & (out->alignment - 1)) || load_base > UINT64_MAX - out->size) {
        goto invalid;
    }
    r.delta = load_base - out->base;
    out->data = fw_alloc(out->size);
    r.fixed = fw_alloc(out->size);
    if (!out->data || !r.fixed)
        goto invalid;
    for (i = 0; i < phnum; i++) {
        const uint8_t *p = h + phoff + i * sizeof(Elf64_Phdr);
        IA64FirmwareSegment s;
        unsigned j;

        if (ldl_le_p(p) != PT_LOAD) {
            continue;
        }
        s.offset = ldq_le_p(p + 16) - out->base;
        s.size = ldq_le_p(p + 40);
        for (j = 0; j < out->segment_count; j++) {
            IA64FirmwareSegment *other = &out->segments[j];
            if (s.offset < other->offset + other->size && other->offset < s.offset + s.size) {
                goto invalid;
            }
        }
        if (out->segment_count == 64)
            goto invalid;
        out->segments[out->segment_count++] = s;
        memcpy(out->data + s.offset, h + ldq_le_p(p + 8), ldq_le_p(p + 32));
    }
    r.section_count = shnum;
    r.sections = fw_alloc(sizeof(Elf64_Shdr) * shnum);
    if (!r.sections)
        goto invalid;
    for (i = 0; i < shnum; i++) {
        const uint8_t *p = h + shoff + i * sizeof(Elf64_Shdr);
        Elf64_Shdr *s = &r.sections[i];

        s->sh_name = ldl_le_p(p);
        s->sh_type = ldl_le_p(p + 4);
        s->sh_flags = ldq_le_p(p + 8);
        s->sh_addr = ldq_le_p(p + 16);
        s->sh_offset = ldq_le_p(p + 24);
        s->sh_size = ldq_le_p(p + 32);
        s->sh_link = ldl_le_p(p + 40);
        s->sh_info = ldl_le_p(p + 44);
        s->sh_addralign = ldq_le_p(p + 48);
        s->sh_entsize = ldq_le_p(p + 56);
        if ((s->sh_type != SHT_NOBITS && !file_range(&r, s->sh_offset, s->sh_size)) ||
            ((s->sh_flags & SHF_ALLOC) &&
             (!memory_range(&r, s->sh_addr, s->sh_size) || (s->sh_flags & FW_SHF_TLS)))) {
            goto invalid;
        }
    }
    if (r.sections[names].sh_type != SHT_STRTAB) {
        goto invalid;
    }
    r.names = (const char *)h + r.sections[names].sh_offset;
    r.names_size = r.sections[names].sh_size;
    for (i = 0; i < shnum; i++) {
        uint32_t n = r.sections[i].sh_name;
        if (n >= r.names_size || !has_zero(r.names + n, r.names_size - n)) {
            goto invalid;
        }
    }
    out->entry = ldq_le_p(h + 24);
    if (!executable_address(&r, out->entry) || !find_symbol(&r, "__gp", &out->global_pointer) ||
        !find_symbol(&r, "pal_proc_entry", &out->pal_entry) ||
        !find_symbol(&r, "__firmware_payload_end", &payload_end) || payload_end != end ||
        !executable_address(&r, out->pal_entry)) {
        goto invalid;
    }
    /* Relocation checks must not depend on earlier output writes. */
    r.linked = fw_alloc(out->size);
    if (!r.linked)
        goto invalid;
    memcpy(r.linked, out->data, out->size);
    if (!relocate(&r)) {
        goto done;
    }
    /* Symbols still have linked addresses: test the marker before changing
     * the output range to its relocated base. */
    uint64_t native_pal;
    if (!find_symbol(&r, "__firmware_native_pal", &native_pal))
        goto invalid;
    /* The native PAL's fixed INIT and machine check entrypoints, the role of
     * PAL_A; without them INIT and machine checks stop. */
    if (((find_symbol(&r, "pale_init", &out->pale_init) &&
          !executable_address(&r, out->pale_init)) ||
         (find_symbol(&r, "pale_check", &out->pale_check) &&
          !executable_address(&r, out->pale_check))))
        goto invalid;
    if (out->pale_init)
        out->pale_init += r.delta;
    if (out->pale_check)
        out->pale_check += r.delta;
    out->base = load_base;
    out->entry += r.delta;
    out->global_pointer += r.delta;
    out->pal_entry += r.delta;
    /* A native PAL is guest IA-64 code. Do not authorize the break portal
     * for a firmware advertising the native PAL marker, even at its entry. */
    out->pal_entry = 0;
    success = true;
    goto done;

invalid:
    LOGE("invalid IA-64 firmware ELF layout");
done:
    fw_free(r.linked);
    fw_free(r.fixed);
    fw_free(r.sections);
    if (!success) {
        ia64_firmware_elf_clear(out);
    }
    return success;
}

bool fw_load(const void *elf, size_t size, uint64_t base, struct fw_image *out)
{
    IA64FirmwareElf image;
    memset(out, 0, sizeof(*out));
    if (!elf || !parse_elf(elf, size, base, &image))
        return false;
    bool ok = platform_ram_write(image.base, image.data, image.size);
    if (ok) {
        *out = (struct fw_image){image.base,      image.size,           image.alignment,
                                 image.entry,     image.global_pointer, image.pal_entry,
                                 image.pale_init, image.pale_check};
        LOGI(
            "firmware base=%llx size=%llx entry=%llx gp=%llx pal=%llx pale_init=%llx pale_check=%llx",
            (unsigned long long)out->base, (unsigned long long)out->size,
            (unsigned long long)out->entry, (unsigned long long)out->global_pointer,
            (unsigned long long)out->pal_entry, (unsigned long long)out->pale_init,
            (unsigned long long)out->pale_check);
    }
    ia64_firmware_elf_clear(&image);
    return ok;
}
