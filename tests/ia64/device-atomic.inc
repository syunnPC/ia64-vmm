/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Semaphores and 16-byte references to device pages. Non-WB pages raise
 * Unsupported Data Reference before any reference, after Unaligned Data
 * Reference (SDM Vol.2 §4.4.9, Table 5-6, the Unsupported Data Reference
 * vector); fetchadd to UCE faults on models without exported fetchadd.
 * A WB page that maps a device reads the datum once and writes it at most
 * once, as the semaphore requires (Vol.3 Table 3-1 mem_xchg, mem_xchg_add,
 * mem_xchg_cond); 16-byte references are two 8-byte transactions, lower
 * address first (mem_read_pair, mem_write16, mem_xchg16_cond). With
 * UM.be = 1 the device receives the bytes in memory order (Vol.1 §3.2.3). */
#define DEVICE (ENTRY + 0x20000)
#define DEVICE_WORD UINT64_C(0xfedcba9876543210)

static struct ia64_result device_execute(struct vcpu *v, Ia64Opcode opcode,
                                         uint64_t address, int64_t immediate)
{
    Ia64Instruction i = instruction(opcode); i.unit = IA64_UNIT_M;
    i.operands.memory.immediate = immediate;
    v->s.gr[10] = address; mmio.logged = 0;
    return ia64_execute(v, &i);
}
static bool device_access(unsigned at, bool write, uint64_t offset, unsigned size, uint64_t value)
{
    const struct mmio_access *a = &mmio.log[at];
    return mmio.logged > at && a->write == write && a->offset == offset && a->size == size &&
           (!write || a->value == value);
}

/* UM.be = 1 on the direct device range: every datum, and each 8-byte half
 * of a 16-byte reference, is stored most significant byte first. */
static bool direct_bytes(unsigned offset, const uint8_t *expected, unsigned size)
{
    return !memcmp(direct_mmio + offset, expected, size);
}
static void test_direct_big_endian(struct vcpu *v)
{
    const uint64_t direct = ENTRY + 0x40000;
    uint64_t psr = v->s.psr, gr9 = v->s.gr[9];
    v->s.psr |= IA64_PSR_BE;
    memcpy(direct_mmio + 32, (const uint8_t[]){1, 2, 3, 4, 0xff, 0xff, 0xff, 0xff}, 8);
    memcpy(direct_mmio + 40, (const uint8_t[]){0, 0, 0, 0, 0, 0, 1, 0}, 8);
    v->s.gr[9] = 0x11223344;
    CHECK(device_execute(v, IA64_OP_XCHG4, direct + 32, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0x01020304 && direct_bytes(32, (const uint8_t[]){0x11, 0x22, 0x33, 0x44}, 4));
    CHECK(device_execute(v, IA64_OP_FETCHADD4, direct + 36, 1).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0xffffffff && direct_bytes(36, (const uint8_t[]){0, 0, 0, 0}, 4));
    CHECK(device_execute(v, IA64_OP_FETCHADD8, direct + 40, -16).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0x100 && direct_bytes(40, (const uint8_t[]){0, 0, 0, 0, 0, 0, 0, 0xf0}, 8));
    v->s.ar[AR_CCV] = UINT64_C(0x111223344);
    CHECK(device_execute(v, IA64_OP_CMPXCHG4, direct + 32, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0x11223344 && direct_bytes(32, (const uint8_t[]){0x11, 0x22, 0x33, 0x44}, 4));
    v->s.ar[AR_CCV] = 0xf0;
    CHECK(device_execute(v, IA64_OP_CMPXCHG8, direct + 40, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0xf0 && direct_bytes(40, (const uint8_t[]){0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44}, 8));
    v->s.gr[9] = UINT64_C(0x0102030405060708); v->s.ar[AR_CSD] = UINT64_C(0x1112131415161718);
    CHECK(device_execute(v, IA64_OP_ST16, direct + 48, 0).kind == IA64_STEP_COMPLETE);
    CHECK(direct_bytes(48, (const uint8_t[]){1, 2, 3, 4, 5, 6, 7, 8,
                                             0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18}, 16));
    v->s.ar[AR_CSD] = 0;
    CHECK(device_execute(v, IA64_OP_LD16, direct + 48, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == UINT64_C(0x0102030405060708) && v->s.ar[AR_CSD] == UINT64_C(0x1112131415161718));
    v->s.ar[AR_CCV] = UINT64_C(0x1112131415161718);
    v->s.gr[9] = UINT64_C(0xa1a2a3a4a5a6a7a8); v->s.ar[AR_CSD] = UINT64_C(0x2122232425262728);
    CHECK(device_execute(v, IA64_OP_CMP8XCHG16, direct + 56, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == UINT64_C(0x1112131415161718));
    CHECK(direct_bytes(48, (const uint8_t[]){0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
                                             0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28}, 16));
    v->s.ar[AR_CCV] = 1; v->s.gr[9] = 0;
    CHECK(device_execute(v, IA64_OP_CMP8XCHG16, direct + 48, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == UINT64_C(0xa1a2a3a4a5a6a7a8));
    CHECK(direct_bytes(48, (const uint8_t[]){0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
                                             0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28}, 16));
    v->s.psr = psr; v->s.gr[9] = gr9;
}

static void test_device_atomics(void)
{
    struct vcpu *v = new_vcpu();
    const uint64_t uc = UINT64_C(1) << 63;
    v->s.gr[9] = UINT64_C(0x1122334455667788);
    CHECK(device_execute(v, IA64_OP_XCHG8, DEVICE + 8, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == DEVICE_WORD && mmio.logged == 2);
    CHECK(device_access(0, false, 8, 8, 0) && device_access(1, true, 8, 8, v->s.gr[9]));
    v->s.ar[AR_CCV] = 0x76543210;
    CHECK(device_execute(v, IA64_OP_CMPXCHG4, DEVICE + 4, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0x76543210 && mmio.logged == 2);
    CHECK(device_access(0, false, 4, 4, 0) && device_access(1, true, 4, 4, 0x55667788));
    v->s.ar[AR_CCV] = 0x76543211;
    CHECK(device_execute(v, IA64_OP_CMPXCHG4, DEVICE + 4, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0x76543210 && mmio.logged == 1 && device_access(0, false, 4, 4, 0));
    v->s.ar[AR_CCV] = 0x13210;
    CHECK(device_execute(v, IA64_OP_CMPXCHG2, DEVICE + 2, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0x3210 && mmio.logged == 1 && device_access(0, false, 2, 2, 0));
    CHECK(device_execute(v, IA64_OP_FETCHADD8, DEVICE, -16).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == DEVICE_WORD && mmio.logged == 2);
    CHECK(device_access(0, false, 0, 8, 0) && device_access(1, true, 0, 8, DEVICE_WORD - 16));
    /* A direct device range takes the same single read and write. */
    uint64_t old = UINT64_C(0x0123456789abcdef);
    memcpy(direct_mmio + 16, &old, 8);
    CHECK(device_execute(v, IA64_OP_XCHG8, ENTRY + 0x40000 + 16, 0).kind == IA64_STEP_COMPLETE);
    uint64_t now; memcpy(&now, direct_mmio + 16, 8);
    CHECK(v->s.gr[8] == old && now == v->s.gr[9]);
    test_direct_big_endian(v);

    /* Non-WB attributes: no device reference at all, even without a region. */
    v->s.gr[8] = 0x55;
    CHECK(device_execute(v, IA64_OP_XCHG8, uc | DEVICE, 0).fault == IA64_FAULT_UNSUPPORTED_DATA);
    CHECK(device_execute(v, IA64_OP_FETCHADD4, uc | DEVICE, 1).fault == IA64_FAULT_UNSUPPORTED_DATA);
    CHECK(device_execute(v, IA64_OP_CMPXCHG8, uc | UINT64_C(0x7f000000), 0).fault == IA64_FAULT_UNSUPPORTED_DATA);
    CHECK(device_execute(v, IA64_OP_XCHG8, uc | (DEVICE + 4), 0).fault == IA64_FAULT_UNALIGNED);
    CHECK(device_execute(v, IA64_OP_LD16, uc | DEVICE, 0).fault == IA64_FAULT_UNSUPPORTED_DATA);
    CHECK(device_execute(v, IA64_OP_LD16, uc | (DEVICE + 8), 0).fault == IA64_FAULT_UNALIGNED);
    CHECK(device_execute(v, IA64_OP_ST16, uc | (DEVICE + 8), 0).fault == IA64_FAULT_UNALIGNED);
    CHECK(device_execute(v, IA64_OP_CMP8XCHG16, uc | (DEVICE + 4), 0).fault == IA64_FAULT_UNALIGNED);
    CHECK(device_execute(v, IA64_OP_ST16, uc | DEVICE, 0).fault == IA64_FAULT_UNSUPPORTED_DATA);
    CHECK(device_execute(v, IA64_OP_CMP8XCHG16, uc | (DEVICE + 8), 0).fault == IA64_FAULT_UNSUPPORTED_DATA);
    CHECK(mmio.logged == 0 && v->s.gr[8] == 0x55);
    const uint64_t va = (UINT64_C(1) << 61) | 0x400000;
    v->s.rr[1] = (2u << 8) | (12u << 2);
    v->s.psr = IA64_PSR_DT | IA64_PSR_IC;
    const struct { unsigned ma; enum ia64_fault_kind fault; } attributes[] = {
        {IA64_MA_UCE, IA64_FAULT_UNSUPPORTED_DATA}, {IA64_MA_WC, IA64_FAULT_UNSUPPORTED_DATA},
        {IA64_MA_NAT, IA64_FAULT_DATA_NAT},
    };
    for (unsigned k = 0; k < IA64VMM_ARRAY_SIZE(attributes); ++k) {
        mmu_map(v, 1, 0, va, DEVICE, 12, 0, 0x7e1 | (attributes[k].ma << 2));
        CHECK(device_execute(v, IA64_OP_FETCHADD8, va, 1).fault == attributes[k].fault);
        CHECK(device_execute(v, IA64_OP_XCHG4, va, 0).fault == attributes[k].fault);
        CHECK(device_execute(v, IA64_OP_CMP8XCHG16, va + 8, 0).fault == attributes[k].fault);
        CHECK(mmio.logged == 0 && v->s.gr[8] == 0x55);
    }
    mmu_map(v, 1, 0, va, DEVICE, 12, 0, 0x7e1);
    CHECK(device_execute(v, IA64_OP_XCHG4, va + 12, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0x76543210 && device_access(1, true, 12, 4, 0x55667788));

    /* 16 bytes: two 8-byte transactions, lower address first. */
    v->s.ar[AR_CSD] = UINT64_C(0x99aabbccddeeff00);
    CHECK(device_execute(v, IA64_OP_ST16, va + 16, 0).kind == IA64_STEP_COMPLETE && mmio.logged == 2);
    CHECK(device_access(0, true, 16, 8, v->s.gr[9]) && device_access(1, true, 24, 8, v->s.ar[AR_CSD]));
    v->s.ar[AR_CSD] = 0;
    CHECK(device_execute(v, IA64_OP_LD16, va + 16, 0).kind == IA64_STEP_COMPLETE && mmio.logged == 2);
    CHECK(device_access(0, false, 16, 8, 0) && device_access(1, false, 24, 8, 0));
    CHECK(v->s.gr[8] == DEVICE_WORD && v->s.ar[AR_CSD] == DEVICE_WORD);
    v->s.ar[AR_CCV] = DEVICE_WORD; v->s.ar[AR_CSD] = 7;
    CHECK(device_execute(v, IA64_OP_CMP8XCHG16, va + 24, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == DEVICE_WORD && mmio.logged == 3 && device_access(0, false, 24, 8, 0));
    CHECK(device_access(1, true, 16, 8, v->s.gr[9]) && device_access(2, true, 24, 8, 7));
    v->s.ar[AR_CCV] = 1;
    CHECK(device_execute(v, IA64_OP_CMP8XCHG16, va + 16, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == DEVICE_WORD && mmio.logged == 1 && device_access(0, false, 16, 8, 0));

    /* UM.be = 1: memory order is reversed within each datum. */
    v->s.psr |= IA64_PSR_BE;
    v->s.gr[9] = 0x11223344;
    CHECK(device_execute(v, IA64_OP_ST4, va + 4, 0).kind == IA64_STEP_COMPLETE);
    CHECK(mmio.logged == 1 && device_access(0, true, 4, 4, 0x44332211));
    CHECK(device_execute(v, IA64_OP_LD4, va + 4, 0).kind == IA64_STEP_COMPLETE && v->s.gr[8] == 0x10325476);
    CHECK(device_execute(v, IA64_OP_XCHG4, va + 4, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0x10325476 && device_access(1, true, 4, 4, 0x44332211));
    CHECK(device_execute(v, IA64_OP_FETCHADD4, va + 4, 1).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0x10325476 && device_access(1, true, 4, 4, 0x77543210));
    v->s.ar[AR_CCV] = 0x10325476;
    CHECK(device_execute(v, IA64_OP_CMPXCHG4, va + 4, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == 0x10325476 && device_access(1, true, 4, 4, 0x44332211));
    v->s.ar[AR_CSD] = UINT64_C(0x0102030405060708);
    CHECK(device_execute(v, IA64_OP_ST16, va + 16, 0).kind == IA64_STEP_COMPLETE);
    CHECK(device_access(0, true, 16, 8, UINT64_C(0x4433221100000000)) &&
          device_access(1, true, 24, 8, UINT64_C(0x0807060504030201)));
    CHECK(device_execute(v, IA64_OP_LD16, va + 16, 0).kind == IA64_STEP_COMPLETE);
    CHECK(v->s.gr[8] == UINT64_C(0x1032547698badcfe) && v->s.ar[AR_CSD] == UINT64_C(0x1032547698badcfe));
    vcpu_destroy(v);
}

/* Two vCPUs on two host threads: fetchadd to a device page mapped WB loses
 * no update, although the device serializes single accesses only. */
#define COUNTER (ENTRY + 0x50000)
#define COUNTER_ADDS 20000u
struct counter_device { unsigned lock; uint64_t value; };
static struct counter_device counter;
static void counter_lock(struct counter_device *c)
{
    while (__atomic_exchange_n(&c->lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}
static uint64_t counter_read(void *opaque, struct vcpu *v, uint64_t offset, unsigned size)
{
    (void)v; (void)offset; (void)size;
    struct counter_device *c = opaque; counter_lock(c);
    uint64_t value = c->value;
    __atomic_store_n(&c->lock, 0, __ATOMIC_RELEASE);
    return value;
}
static void counter_write(void *opaque, struct vcpu *v, uint64_t offset, uint64_t value, unsigned size)
{
    (void)v; (void)offset; (void)size;
    struct counter_device *c = opaque; counter_lock(c);
    c->value = value;
    __atomic_store_n(&c->lock, 0, __ATOMIC_RELEASE);
}
static const struct gm_mmio_ops counter_ops = {counter_read, counter_write};
static void *counter_thread(void *arg)
{
    struct vcpu *v = arg;
    Ia64Instruction i = instruction(IA64_OP_FETCHADD8); i.unit = IA64_UNIT_M;
    i.operands.memory.immediate = 1;
    for (unsigned n = 0; n < COUNTER_ADDS; ++n) {
        v->s.gr[10] = COUNTER;
        CHECK(ia64_execute(v, &i).kind == IA64_STEP_COMPLETE);
    }
    return NULL;
}
static void test_device_atomic_threads(void)
{
    struct vcpu *v[2] = {new_vcpu(), new_vcpu()};
    pthread_t thread[2];
    for (unsigned k = 0; k < 2; ++k) CHECK(!pthread_create(&thread[k], NULL, counter_thread, v[k]));
    for (unsigned k = 0; k < 2; ++k) CHECK(!pthread_join(thread[k], NULL));
    CHECK(counter.value == 2 * COUNTER_ADDS);
    for (unsigned k = 0; k < 2; ++k) vcpu_destroy(v[k]);
}
