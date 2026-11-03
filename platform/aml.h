/* SPDX-License-Identifier: GPL-3.0-or-later AND Apache-2.0 */
/* Copyright 2026 syunnPC
 * Origin: Asteria Source/Acpi/Aml/{Parser,Namespace,Evaluator,Objects,Facts,Resources}.cpp,
 * Source/Acpi/Aml/{Types,Library,Resources}.hpp
 */
#ifndef VMM_AML_H
#define VMM_AML_H
#include "ia64vmm/types.h"
/* Bounded routing subset. No hardware-dependent value is guessed.
 * Tables are borrowed for the context's lifetime; evaluate only at setup. */
#define AML_NODES 4096u
#define AML_OBJECTS 16384u
#define AML_ELEMENTS 16384u
#define AML_HEAP 16384u
enum aml_kind { AML_UNKNOWN, AML_INTEGER, AML_STRING, AML_BUFFER, AML_PACKAGE, AML_REFERENCE };
struct aml_object {
    enum aml_kind kind;
    uint64_t integer;
    const uint8_t *bytes;
    unsigned length, first;
};
/* target: a field unit's region node, a buffer field's buffer object, or
 * BDF + 1 of a PCI_Config region whose device is resolved (0 otherwise). */
struct aml_node {
    unsigned parent, segment, opcode, flags, value;
    const uint8_t *begin, *end;
    bool busy, integer32;
    unsigned target, bit, width;
};
struct aml_context {
    struct aml_node nodes[AML_NODES];
    struct aml_object objects[AML_OBJECTS];
    unsigned elements[AML_ELEMENTS];
    uint8_t heap[AML_HEAP]; /* Buffers written through buffer fields. */
    unsigned nnodes, nobjects, nelements, nheap, operations, error, evaluation_nodes,
        evaluation_objects;
    unsigned journal[AML_NODES];
    uint64_t integer_mask;
    bool load32;
    /* PCI_Config field reads; set after aml_init. */
    uint32_t (*config_read)(void *, uint16_t bdf, unsigned offset, unsigned size);
    void *config_context;
};
void aml_init(struct aml_context *);
bool aml_load(struct aml_context *, const uint8_t *, size_t);
unsigned aml_child(struct aml_context *, unsigned parent, const char name[4]);
unsigned aml_evaluate(struct aml_context *, unsigned node, const unsigned *args, unsigned count);
unsigned aml_integer(struct aml_context *, uint64_t);
const struct aml_object *aml_object(struct aml_context *, unsigned);
bool aml_irq_resource(const struct aml_object *, unsigned index, uint32_t *gsi, bool *low,
                      bool *level);
#endif
