/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Tests/Acpi/Aml/{LibraryTests,QemuTablesTests,ResourcesTests}.cpp
 */
/* Applicable routing/parser/evaluator tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/acpi-irq.h"
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            fprintf(stderr, "%s:%u %s\n", __FILE__, __LINE__, #e);                                 \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
/* Configuration spaces behind PCI_Config regions: the QEMU pc PIIX ISA
 * bridge (00:01.0) and the core fixture's ISA0 (00:02.1). PIIX4 PIRQRC
 * (60h-63h): bit 7 disables routing, bits 3:0 select the IRQ. */
static uint8_t config[2][256];
static unsigned config_reads, config_sizes;
static uint32_t read_cfg(void *p, uint16_t bdf, unsigned reg, unsigned size)
{
    (void)p;
    const uint8_t *space = bdf == 0x0008 ? config[0] : bdf == 0x0011 ? config[1] : NULL;
    if (!space)
        return 0xffffffff;
    CHECK((size == 1 || size == 2 || size == 4) && reg % size == 0 && reg + size <= 256);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; ++i)
        v |= (uint32_t)space[reg + i] << (8 * i);
    ++config_reads;
    config_sizes |= size;
    return v;
}
static uint64_t integer(struct aml_context *a, const char *name)
{
    unsigned id = aml_evaluate(a, aml_child(a, 1, name), NULL, 0);
    CHECK(!a->error && aml_object(a, id)->kind == AML_INTEGER);
    return aml_object(a, id)->integer;
}
int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    struct acpi_irq *c = calloc(1, sizeof(*c));
    CHECK(c);
    aml_init(&c->aml);
    uint8_t *tables[256] = {0};
    CHECK(argc <= 257);
    for (int i = 1; i < argc; ++i) {
        FILE *f = fopen(argv[i], "rb");
        CHECK(f);
        CHECK(!fseek(f, 0, SEEK_END));
        long n = ftell(f);
        CHECK(n >= 36 && n <= 1048576);
        rewind(f);
        tables[i - 1] = malloc(n);
        CHECK(tables[i - 1]);
        CHECK(fread(tables[i - 1], 1, n, f) == (size_t)n);
        fclose(f);
        CHECK(aml_load(&c->aml, tables[i - 1], n));
    }
    const struct pci_host_ops ops = {.read = read_cfg};
    static const uint8_t pirq[] = {0x0a, 0x0b, 0x85, 0x05}, isa0[] = {0x0b, 0x8a, 0x34, 0x05};
    memcpy(config[0] + 0x60, pirq, 4);
    memcpy(config[1] + 0x60, isa0, 4);
    CHECK(acpi_irq_prepare(c, &ops, NULL));
    CHECK(c->nroutes > 0);
    uint32_t gsi;
    bool low, level;
    if (strstr(argv[1], "aml-core")) {
        CHECK(c->nroutes == 1 && c->rejected == 1);
        CHECK(acpi_irq_resolve(c, 1u << 3, 1, &gsi, &low, &level));
        CHECK(gsi == 17 && low && level);
        unsigned args[] = {aml_integer(&c->aml, 7), aml_integer(&c->aml, 9)};
        unsigned result = aml_evaluate(&c->aml, aml_child(&c->aml, 1, "CALC"), args, 2);
        CHECK(aml_object(&c->aml, result)->kind == AML_INTEGER &&
              aml_object(&c->aml, result)->integer == 53);
        unsigned booleans[] = {aml_integer(&c->aml, 0), aml_integer(&c->aml, 7)};
        result = aml_evaluate(&c->aml, aml_child(&c->aml, 1, "BOOL"), booleans, 2);
        const struct aml_object *logical = aml_object(&c->aml, result);
        CHECK(logical->kind == AML_PACKAGE && logical->length == 6);
        const bool truth[] = {true, false, true, false, false, true};
        for (unsigned i = 0; i < 6; ++i)
            CHECK(aml_object(&c->aml, c->aml.elements[logical->first + i])->integer ==
                  (truth[i] ? UINT64_MAX : 0));
        CHECK(!aml_evaluate(&c->aml, aml_child(&c->aml, 1, "RECU"), NULL, 0) && c->aml.error);
        CHECK(!aml_evaluate(&c->aml, aml_child(&c->aml, 1, "FLDR"), NULL, 0) && c->aml.error);
        CHECK(!aml_evaluate(&c->aml, aml_child(&c->aml, 1, "FAIL"), NULL, 0) && c->aml.error);
        result = aml_evaluate(&c->aml, aml_child(&c->aml, 1, "MODE"), NULL, 0);
        CHECK(aml_object(&c->aml, result)->integer == 1); /* Failed Store rolled back. */
        struct aml_context *revision1 = calloc(1, sizeof(*revision1));
        CHECK(revision1);
        unsigned length = tables[0][4] | (unsigned)tables[0][5] << 8;
        tables[0][8] = 1;
        ++tables[0][9]; /* Integer width follows DSDT revision. */
        aml_init(revision1);
        CHECK(aml_load(revision1, tables[0], length));
        unsigned wrap[] = {aml_integer(revision1, UINT32_MAX), aml_integer(revision1, 1)};
        result = aml_evaluate(revision1, aml_child(revision1, 1, "CALC"), wrap, 2);
        CHECK(aml_object(revision1, result)->integer == 5);
        unsigned bool32[] = {aml_integer(revision1, 0), aml_integer(revision1, 7)};
        result = aml_evaluate(revision1, aml_child(revision1, 1, "BOOL"), bool32, 2);
        logical = aml_object(revision1, result);
        CHECK(logical->kind == AML_PACKAGE && logical->length == 6);
        for (unsigned i = 0; i < 6; ++i)
            CHECK(aml_object(revision1, revision1->elements[logical->first + i])->integer ==
                  (truth[i] ? UINT32_MAX : 0));
        free(revision1);
        static const uint8_t resources[] = {0x47, 1,    0x60, 0,  0x60, 0, 1, 1,    0x89, 6,
                                            0,    0x0d, 1,    37, 0,    0, 0, 0x79, 0};
        struct aml_object buffer = {
            .kind = AML_BUFFER, .bytes = resources, .length = sizeof(resources)};
        CHECK(!aml_irq_resource(&buffer, 0, &gsi, &low, &level));
        CHECK(aml_irq_resource(&buffer, 1, &gsi, &low, &level) && gsi == 37 && low && level);
        buffer.length = 16;
        CHECK(!aml_irq_resource(&buffer, 1, &gsi, &low, &level));
        /* ACPI 6.5 §19.6.100: PCI_Config regions address the configuration
         * space of the device whose _ADR encloses them, at the field's
         * access width (§19.6.48); fields are read-only in this subset. */
        config_reads = config_sizes = 0;
        CHECK(integer(&c->aml, "RDPD") == 0x05 && config_reads == 1 && config_sizes == 1);
        config_reads = config_sizes = 0;
        CHECK(integer(&c->aml, "RDPW") == 0x48a && config_reads == 2 && config_sizes == 2);
        CHECK(!aml_evaluate(&c->aml, aml_child(&c->aml, 1, "WRPQ"), NULL, 0) && c->aml.error);
        /* §19.6.81/§19.6.21: a method's Name and CreateDWordField objects
         * are created per invocation; the table's template stays unchanged. */
        unsigned first = aml_evaluate(&c->aml, aml_child(&c->aml, 1, "LNKR"), NULL, 0);
        CHECK(!c->aml.error && aml_irq_resource(aml_object(&c->aml, first), 0, &gsi, &low, &level));
        CHECK(gsi == 0x0b && level && !low);
        unsigned off[] = {aml_integer(&c->aml, 0x80)};
        unsigned second = aml_evaluate(&c->aml, aml_child(&c->aml, 1, "IRQC"), off, 1);
        CHECK(!c->aml.error && second != first);
        CHECK(aml_irq_resource(aml_object(&c->aml, second), 0, &gsi, &low, &level) && gsi == 0);
        CHECK(aml_irq_resource(aml_object(&c->aml, first), 0, &gsi, &low, &level) && gsi == 0x0b);
        CHECK(!aml_child(&c->aml, aml_child(&c->aml, 1, "IRQC"), "BUF0"));
        /* Evaluation is pure: a named buffer outside the method is not written. */
        CHECK(!aml_evaluate(&c->aml, aml_child(&c->aml, 1, "GBFW"), NULL, 0) && c->aml.error);
        const struct aml_object *global =
            aml_object(&c->aml, aml_evaluate(&c->aml, aml_child(&c->aml, 1, "GBUF"), NULL, 0));
        CHECK(global->kind == AML_BUFFER && global->length == 4 && global->bytes[1] == 2);
        tables[0][9] ^= 1;
        CHECK(!aml_load(&c->aml, tables[0], (size_t)(tables[0][4] | tables[0][5] << 8)));
    } else if (strstr(argv[1], "x86/pc/")) {
        /* QEMU pc _PRT: device 1 INTA is LNKS (SCI, GSI 9); otherwise
         * (device + pin) & 3 selects LNKD/LNKA/LNKB/LNKC, whose _CRS reports
         * PIRQD/A/B/C. A link with routing disabled (_STA not enabled) has
         * no current GSI and stays unresolved. */
        CHECK(c->nroutes == 96 && c->rejected == 32);
        for (unsigned device = 0; device < 32; ++device)
            for (unsigned pin = 1; pin <= 4; ++pin) {
                unsigned link = (device + pin - 1) & 3, value = pirq[(link + 3) & 3];
                bool sci = device == 1 && pin == 1;
                if (!sci && (value & 0x80)) {
                    CHECK(!acpi_irq_resolve(c, device << 3, pin, &gsi, &low, &level));
                    continue;
                }
                CHECK(acpi_irq_resolve(c, device << 3, pin, &gsi, &low, &level));
                CHECK(level && !low && gsi == (sci ? 9 : value));
            }
    } else if (strstr(argv[1], "q35") || strstr(argv[1], "vmware")) {
        bool q35 = strstr(argv[1], "q35") != NULL;
        CHECK(c->nroutes == 128 && c->rejected == 0);
        for (unsigned device = 0; device < 32; ++device)
            for (unsigned pin = 1; pin <= 4; ++pin) {
                CHECK(acpi_irq_resolve(c, device << 3, pin, &gsi, &low, &level));
                CHECK(level && low == !q35 && gsi >= 16 && gsi <= 23);
            }
    }
    CHECK(!acpi_irq_resolve(c, 0, 0, &gsi, &low, &level));
    CHECK(!acpi_irq_resolve(c, 0, 5, &gsi, &low, &level));
    printf("AML routing PASS: %s routes=%u unresolved=%u\n", argv[1], c->nroutes, c->rejected);
    for (int i = 0; i < argc - 1; ++i)
        free(tables[i]);
    free(c);
    return 0;
}
