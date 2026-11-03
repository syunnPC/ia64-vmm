/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Interrupt behaviour app: the new firmware's CPU Architectural Protocol owns the
 * IVT; the app registers vectors without replacing or patching the IVT.
 * IDENTIFY commands are read-only. DMA remains in guest-allocated RAM. */
#include <Uefi.h>
#include <Protocol/Cpu.h>
#ifdef IA64VMM_IRQ_SMP
#include <Guid/Acpi.h>
#include <Guid/SalSystemTable.h>
#endif
#include <ia64vmm/platform.h>
static EFI_SYSTEM_TABLE *system_table;
static volatile UINT64 count[3];
static UINT64 ahci, nvme, msi_address, nvme_cq, device_lid;
static unsigned ahci_port;
static BOOLEAN ahci_intx;
static volatile UINT64 ipi_delivered;
static VOID text(const CHAR8 *s)
{
    while (*s)
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_CONSOLE) = (UINT8)*s++;
}
static VOID check(BOOLEAN condition, const CHAR8 *reason)
{
    if (condition)
        return;
    text("interrupts FAIL: ");
    text(reason);
    text("\n");
    system_table->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_ABORTED, 0, NULL);
    for (;;)
        __asm__ volatile("hint @pause");
}
static UINT64 itc(VOID)
{
    UINT64 value;
    __asm__ volatile("mov %0=ar.itc" : "=r"(value));
    return value;
}
static UINT32 read32(UINT64 base, UINTN off)
{
    UINT32 value = *(volatile UINT32 *)(base + off);
    __asm__ volatile("mf" ::: "memory");
    return value;
}
#ifdef IA64VMM_IRQ_SMP
/* BSP request -> AP1 register write -> BSP acknowledgement. IA-64 mf
 * orders the payload and completion; only one request is outstanding. */
static volatile UINT64 ap_write_address, ap_write_value, ap_write_size, ap_write_request;
static volatile UINT64 ap_pci_writes, ap_rte_writes;
static BOOLEAN ap_config_write(UINT64 address, UINT64 value, unsigned size)
{
    UINT64 lid;
    __asm__ volatile("mov %0=cr.lid" : "=r"(lid));
    if (lid == device_lid ||
        !((address >= IA64VMM_IOSAPIC_BASE && address < IA64VMM_IOSAPIC_BASE + 0x1000) ||
          (address >= IA64VMM_ECAM_BASE && address < IA64VMM_ECAM_BASE + IA64VMM_ECAM_SIZE)))
        return FALSE;
    ap_write_address = address;
    ap_write_value = value;
    ap_write_size = size;
    __asm__ volatile("mf" ::: "memory");
    ap_write_request = 1;
    __asm__ volatile("mf" ::: "memory");
    UINT64 start = itc();
    while (ap_write_request) {
        check(itc() - start < IA64VMM_ITC_HZ * 5, "AP configuration write completion");
        __asm__ volatile("mf;; hint @pause" ::: "memory");
    }
    return TRUE;
}
#endif
static VOID write16(UINT64 address, UINT16 value)
{
#ifdef IA64VMM_IRQ_SMP
    if (ap_config_write(address, value, 2))
        return;
#endif
    __asm__ volatile("mf" ::: "memory");
    *(volatile UINT16 *)address = value;
    __asm__ volatile("mf" ::: "memory");
}
static VOID write32(UINT64 base, UINTN off, UINT32 value)
{
#ifdef IA64VMM_IRQ_SMP
    if (ap_config_write(base + off, value, 4))
        return;
#endif
    __asm__ volatile("mf" ::: "memory");
    *(volatile UINT32 *)(base + off) = value;
    __asm__ volatile("mf" ::: "memory");
}
static VOID wait32(UINT64 base, UINTN off, UINT32 mask, UINT32 value, const CHAR8 *reason)
{
    UINT64 start = itc();
    while ((read32(base, off) & mask) != value) {
        check(itc() - start < IA64VMM_ITC_HZ * 5, reason);
        __asm__ volatile("hint @pause");
    }
}
static VOID diagnostic(const CHAR8 *label, UINT64 value)
{
    text(label);
    text("=");
    for (int bit = 60; bit >= 0; bit -= 4) {
        CHAR8 c[2] = {"0123456789abcdef"[(value >> bit) & 15], 0};
        text(c);
    }
    text("\n");
}
static VOID wait_irq(unsigned index, const CHAR8 *reason)
{
    UINT64 start = itc();
    while (!count[index]) {
        if (itc() - start >= IA64VMM_ITC_HZ * 5 && index == 2) {
            diagnostic("NVMe CC", read32(nvme, 0x14));
            diagnostic("NVMe CSTS", read32(nvme, 0x1c));
            diagnostic("NVMe INTMS", read32(nvme, 0x0c));
            diagnostic("NVMe CQ word3", ((volatile UINT32 *)nvme_cq)[3]);
            diagnostic("NVMe CQ word2", ((volatile UINT32 *)nvme_cq)[2]);
        }
        check(itc() - start < IA64VMM_ITC_HZ * 5, reason);
        /* A PAL_HALT_LIGHT would test wake too, but needs the PAL entry;
         * bounded pause works with either engine and wall-clock ITC. */
        __asm__ volatile("hint @pause");
    }
}
static VOID EFIAPI handler(EFI_EXCEPTION_TYPE vector, EFI_SYSTEM_CONTEXT context)
{
    (void)context;
    if (vector >= 0x61 && vector <= 0x63)
        ++count[vector - 0x61];
    if (vector == 0x61)
        ipi_delivered = itc();
    if (vector == 0x62 && ahci) {
        UINTN port = 0x100 + ahci_port * 0x80;
        write32(ahci, port + 0x10, read32(ahci, port + 0x10));
        write32(ahci, 8, read32(ahci, 8));
        if (ahci_intx)
            write32(IA64VMM_IOSAPIC_BASE, 0x40, 0x62);
    }
}
#ifdef IA64VMM_IRQ_SMP
/* SAL/MADT discovery and descriptor call follow tests/efi/smp/smp.c,
 * the SMP application, SAL 245359-005 §3.2.6 and §9. */
static volatile UINT64 ap_ready, ap_stop, ap_done;
static UINT64 little(const UINT8 *p, unsigned n)
{
    UINT64 value = 0;
    for (unsigned i = 0; i < n; ++i)
        value |= (UINT64)p[i] << (i * 8);
    return value;
}
static BOOLEAN bytes_equal(const VOID *a, const VOID *b, unsigned n)
{
    const UINT8 *x = a, *y = b;
    for (unsigned i = 0; i < n; ++i)
        if (x[i] != y[i])
            return FALSE;
    return TRUE;
}
static BOOLEAN checksum(const UINT8 *p, unsigned n)
{
    UINT8 sum = 0;
    for (unsigned i = 0; i < n; ++i)
        sum += p[i];
    return !sum;
}
static const UINT8 *config_table(EFI_GUID guid)
{
    for (UINTN i = 0; i < system_table->NumberOfTableEntries; ++i)
        if (bytes_equal(&guid, &system_table->ConfigurationTable[i].VendorGuid, sizeof(guid)))
            return system_table->ConfigurationTable[i].VendorTable;
    return NULL;
}
static VOID ap_external(UINT64 vector, UINT64 *context)
{
    EFI_SYSTEM_CONTEXT saved;
    saved.SystemContextIpf = (VOID *)context;
    handler(vector, saved);
}
static VOID ap_rendezvous(VOID)
{
    UINT64 lid;
    __asm__ volatile("mov %0=cr.lid" : "=r"(lid));
    if (lid != device_lid) {
        *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_PANIC) = 0x534d5050;
        for (;;)
            __asm__ volatile("hint @pause");
    }
    /* The AP context is its own, initialized by ap_context_init (ap.c).
     * The rendezvous installs its external callback, keeping the firmware
     * IVT save/restore and unaligned handler. It never enters BSP DXE. */
    UINT64 block;
    __asm__ volatile("mov %0=ar.k1" : "=r"(block));
    volatile UINT64 *dispatch = (VOID *)block;
    const UINT64 *entry = (const VOID *)ap_external;
    UINT64 old_entry = dispatch[0], old_gp = dispatch[1];
    dispatch[0] = entry[0];
    dispatch[1] = entry[1];
    __asm__ volatile("mf;; mov cr.tpr=r0;; ssm psr.i;; srlz.d" ::: "memory");
    ap_ready = 1;
    __asm__ volatile("mf" ::: "memory");
    while (!ap_stop) {
        __asm__ volatile("mf" ::: "memory");
        if (ap_write_request) {
            if (ap_write_size == 2)
                *(volatile UINT16 *)ap_write_address = ap_write_value;
            else
                *(volatile UINT32 *)ap_write_address = ap_write_value;
            if (ap_write_address >= IA64VMM_ECAM_BASE &&
                ap_write_address < IA64VMM_ECAM_BASE + IA64VMM_ECAM_SIZE)
                ++ap_pci_writes;
            else
                ++ap_rte_writes;
            __asm__ volatile("mf" ::: "memory");
            ap_write_request = 0;
            __asm__ volatile("mf" ::: "memory");
        }
        __asm__ volatile("hint @pause" ::: "memory");
    }
    __asm__ volatile("rsm psr.i;; srlz.d;; mf" ::: "memory");
    dispatch[0] = old_entry;
    dispatch[1] = old_gp;
    ap_done = 1;
    __asm__ volatile("mf" ::: "memory");
}
static VOID start_ap(VOID)
{
    EFI_GUID acpi_guid = EFI_ACPI_20_TABLE_GUID, sal_guid = SAL_SYSTEM_TABLE_GUID;
    const UINT8 *rsdp = config_table(acpi_guid), *sal = config_table(sal_guid), *madt = NULL;
    check(rsdp && bytes_equal(rsdp, "RSD PTR ", 8), "SMP ACPI RSDP");
    const UINT8 *xsdt = (const VOID *)little(rsdp + 24, 8);
    check(xsdt && bytes_equal(xsdt, "XSDT", 4), "SMP XSDT");
    unsigned length = little(xsdt + 4, 4);
    check(length >= 36 && length < 0x10000 && checksum(xsdt, length), "SMP XSDT checksum");
    for (unsigned off = 36; off + 8 <= length; off += 8) {
        const UINT8 *table = (const VOID *)little(xsdt + off, 8);
        if (bytes_equal(table, "APIC", 4))
            madt = table;
    }
    check(madt != NULL, "SMP MADT");
    length = little(madt + 4, 4);
    check(length >= 44 && length < 0x10000 && checksum(madt, length), "SMP MADT checksum");
    unsigned cpus = 0;
    for (unsigned off = 44; off + 2 <= length;) {
        const UINT8 *e = madt + off;
        unsigned n = e[1];
        check(n >= 2 && n <= length - off, "SMP MADT descriptor");
        if (e[0] == 7 && n >= 12 && (little(e + 8, 4) & 1)) {
            ++cpus;
            if (e[3] == 1)
                device_lid = (UINT64)e[3] << 24 | (UINT64)e[4] << 16;
        }
        off += n;
    }
    check(cpus == 2 && device_lid == (1u << 24), "SMP exactly two vCPUs, target LID1");
    check(sal && bytes_equal(sal, "SST_", 4), "SMP SAL table");
    length = little(sal + 4, 4);
    check(length >= 96 && length < 0x10000 && checksum(sal, length), "SMP SAL checksum");
    static const unsigned sizes[6] = {48, 32, 16, 32, 16, 16};
    static UINT64 descriptor[2] __attribute__((aligned(16)));
    UINT64 wake = 0;
    unsigned off = 96;
    for (unsigned i = 0; i < little(sal + 10, 2); ++i) {
        check(off < length && sal[off] < 6 && sizes[sal[off]] <= length - off,
              "SMP SAL descriptor");
        if (sal[off] == 0) {
            descriptor[0] = little(sal + off + 16, 8);
            descriptor[1] = little(sal + off + 24, 8);
        }
        if (sal[off] == 5) {
            check(sal[off + 1] == 0, "SMP SAL wake mechanism");
            wake = little(sal + off + 8, 8);
        }
        off += sizes[sal[off]];
    }
    check(descriptor[0] && wake == 0xff, "SMP SAL entry/wake vector");
    struct sal_result {
        UINT64 status, v0, v1, v2;
    };
    typedef struct sal_result (*sal_fn)(UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64,
                                        UINT64);
    const UINT64 *entry = (const VOID *)ap_rendezvous;
    UINT64 fn = (UINT64)descriptor;
    __asm__ volatile("" : "+r"(fn));
    struct sal_result result = ((sal_fn)fn)(0x01000000, 2, entry[0], entry[1], 0, 0, 0, 0);
    check(!result.status, "SMP SAL_SET_VECTORS");
    *(volatile UINT64 *)(IA64VMM_PIB_BASE + 0x1000) = wake;
    __asm__ volatile("mf" ::: "memory");
    UINT64 start = itc();
    while (!ap_ready) {
        check(itc() - start < IA64VMM_ITC_HZ * 10, "SMP AP call-in");
        __asm__ volatile("hint @pause" ::: "memory");
    }
    text("SMP AP1 ready PASS\n");
}
static VOID stop_ap(VOID)
{
    ap_stop = 1;
    __asm__ volatile("mf" ::: "memory");
    UINT64 start = itc();
    while (!ap_done) {
        check(itc() - start < IA64VMM_ITC_HZ * 5, "SMP AP stop");
        __asm__ volatile("hint @pause" ::: "memory");
    }
    text("SMP MSI MSI-X INTx target vCPU1 PASS\n");
}
#endif
static UINT64 allocate(unsigned pages)
{
    EFI_PHYSICAL_ADDRESS at = 0;
    check(system_table->BootServices->AllocatePages(AllocateAnyPages, EfiBootServicesData, pages,
                                                    &at) == EFI_SUCCESS,
          "DMA pages");
    for (UINTN i = 0; i < (UINTN)pages * 512; ++i)
        ((UINT64 *)at)[i] = 0;
    return at;
}
static UINT64 function(unsigned dev, unsigned fn)
{
    return IA64VMM_ECAM_BASE + ((UINT64)dev << 15) + ((UINT64)fn << 12);
}
static UINT64 find(UINT32 class_code)
{
    for (unsigned bus = 0; bus < 256; ++bus)
        for (unsigned dev = 0; dev < 32; ++dev) {
            unsigned functions = 1;
            for (unsigned fn = 0; fn < functions; ++fn) {
                UINT64 cfg = function(dev, fn) + ((UINT64)bus << 20);
                if (read32(cfg, 0) == 0xffffffff)
                    continue;
                if (!fn && (read32(cfg, 12) & (1u << 23)))
                    functions = 8;
                if ((read32(cfg, 8) >> 8) == class_code)
                    return cfg;
            }
        }
    return 0;
}
static unsigned capability(UINT64 cfg, unsigned id)
{
    unsigned p = read32(cfg, 0x34) & 0xfc;
    for (unsigned n = 0; n < 48 && p >= 0x40; ++n) {
        UINT32 header = read32(cfg, p);
        if ((header & 255) == id)
            return p;
        p = (header >> 8) & 0xfc;
    }
    return 0;
}
static UINT64 bar(UINT64 cfg, unsigned index)
{
    UINT32 low = read32(cfg, 0x10 + 4 * index);
    UINT64 address = low & ~15u;
    if ((low & 6) == 4)
        address |= (UINT64)read32(cfg, 0x14 + 4 * index) << 32;
    return address;
}
static VOID command(UINT64 cfg)
{
    UINT16 current = *(volatile UINT16 *)(cfg + 4);
    write16(cfg + 4, current | 0x406);
    __asm__ volatile("mf" ::: "memory");
}
static VOID ahci_test(BOOLEAN legacy)
{
    UINT64 cfg = find(0x010601);
    check(cfg != 0, "AHCI assigned controller");
    unsigned cap = capability(cfg, 5);
    check(cap != 0, "AHCI MSI capability");
    ahci = bar(cfg, 5);
    check(ahci != 0, "AHCI BAR5");
    command(cfg);
    count[1] = 0;
    ahci_intx = legacy;
    write32(ahci, 4, (1u << 31) | 1);
    wait32(ahci, 4, 1, 0, "AHCI reset");
    write32(ahci, 4, 1u << 31);
    UINT32 pi = read32(ahci, 0x0c);
    BOOLEAN present = FALSE;
    for (unsigned i = 0; i < 32; ++i)
        if ((pi & (1u << i)) && (read32(ahci, 0x128 + i * 0x80) & 15) == 3) {
            ahci_port = i;
            present = TRUE;
            break;
        }
    check(present, "AHCI SATA test disk");
    UINTN p = 0x100 + ahci_port * 0x80;
    write32(ahci, p + 0x18, 0);
    wait32(ahci, p + 0x18, (1u << 14) | (1u << 15), 0, "AHCI stop");
    UINT64 headers = allocate(1), fis = allocate(1), table = allocate(1), data = allocate(1);
    write32(ahci, p, headers);
    write32(ahci, p + 4, headers >> 32);
    write32(ahci, p + 8, fis);
    write32(ahci, p + 12, fis >> 32);
    UINT32 *h = (VOID *)headers, *t = (VOID *)table;
    h[0] = 5 | (1u << 16);
    h[2] = table;
    h[3] = table >> 32;
    ((UINT8 *)table)[0] = 0x27;
    ((UINT8 *)table)[1] = 0x80;
    ((UINT8 *)table)[2] = 0xec;
    t[32] = data;
    t[33] = data >> 32;
    t[35] = 511 | (1u << 31);
    UINT16 control = *(volatile UINT16 *)(cfg + cap + 2);
    write16(cfg + cap + 2, control & ~0x71u);
    write32(cfg, cap + 4, msi_address);
    unsigned data_off = cap + ((control & 0x80) ? 12 : 8);
    if (control & 0x80)
        write32(cfg, cap + 8, msi_address >> 32);
    write16(cfg + data_off, 0x62);
    if (control & 0x100)
        write32(cfg, data_off + 4, 0);
    if (legacy) {
        unsigned gsi = read32(cfg, 0x3c) & 255;
        check(gsi >= 16 && gsi < IA64VMM_IOSAPIC_GSI_COUNT, "AHCI guest INTx GSI");
        UINT64 lid = device_lid;
        write32(IA64VMM_IOSAPIC_BASE, 0, 0x11 + 2 * gsi);
        write32(IA64VMM_IOSAPIC_BASE, 0x10, lid & 0xffff0000u);
        write32(IA64VMM_IOSAPIC_BASE, 0, 0x10 + 2 * gsi);
        write32(IA64VMM_IOSAPIC_BASE, 0x10, 0x62 | (1u << 15) | (1u << 13));
        write16(cfg + 4, (*(volatile UINT16 *)(cfg + 4) | 6) & ~0x400u);
    } else
        write16(cfg + cap + 2, (control & ~0x70u) | 1);
    write32(ahci, p + 0x10, 0xffffffff);
    write32(ahci, p + 0x30, 0xffffffff);
    write32(ahci, 8, 0xffffffff);
    /* IDENTIFY is PIO data-in; VMware emits only a PIO Setup FIS, while
     * QEMU also emits a Register D2H FIS. Enable both plus PRDT IOC.
     * Behaviour reference: Asteria Source/Kernel/Device/AhciIdentifyCommandExecutor.cpp. */
    write32(ahci, p + 0x14, 0x23);
    write32(ahci, 4, (1u << 31) | 2);
    write32(ahci, p + 0x18, 0x11);
    wait32(ahci, p + 0x20, 0x88, 0, "AHCI taskfile ready");
    write32(ahci, p + 0x38, 1);
    wait_irq(1, legacy ? "AHCI INTx completion interrupt" : "AHCI MSI completion interrupt");
    wait32(ahci, p + 0x38, 1, 0, "AHCI command complete");
    check(h[1] == 512, "AHCI IDENTIFY DMA bytes");
    write16(cfg + cap + 2, control & ~1u);
    command(cfg);
    ahci_intx = FALSE;
    write32(ahci, p + 0x14, 0);
    write32(ahci, 4, 1u << 31);
    text(legacy ? "AHCI INTx vector=98 IDENTIFY interrupt PASS\n"
                : "AHCI MSI vector=98 IDENTIFY interrupt PASS\n");
}
static VOID nvme_test(VOID)
{
    UINT64 cfg = find(0x010802);
    check(cfg != 0, "NVMe assigned controller");
    unsigned cap = capability(cfg, 0x11);
    check(cap != 0, "NVMe MSI-X capability");
    nvme = bar(cfg, 0);
    check(nvme != 0, "NVMe BAR0");
    command(cfg);
    write32(nvme, 0x14, 0);
    wait32(nvme, 0x1c, 1, 0, "NVMe disable");
    UINT64 sq = allocate(1), cq = allocate(1), data = allocate(1);
    nvme_cq = cq;
    write32(nvme, 0x24, 7 | (7u << 16));
    write32(nvme, 0x28, sq);
    write32(nvme, 0x2c, sq >> 32);
    write32(nvme, 0x30, cq);
    write32(nvme, 0x34, cq >> 32);
    write32(nvme, 0x10, 0xffffffffu); /* Clear inherited MSI masks before MSI-X. */
    UINT32 descriptor = read32(cfg, cap + 4);
    UINT64 table = bar(cfg, descriptor & 7) + (descriptor & ~7u);
    write32(table, 12, 1);
    write32(table, 0, msi_address);
    write32(table, 4, msi_address >> 32);
    write32(table, 8, 0x63);
    write32(table, 12, 0);
    UINT16 control = *(volatile UINT16 *)(cfg + cap + 2);
    write16(cfg + cap + 2, (control & ~0x4000u) | 0x8000u);
    write32(nvme, 0x14, 1 | (6u << 16) | (4u << 20));
    wait32(nvme, 0x1c, 1, 1, "NVMe ready");
    UINT32 *cmd = (VOID *)sq;
    cmd[0] = 6 | (1u << 16);
    cmd[6] = data;
    cmd[7] = data >> 32;
    cmd[10] = 1;
    __asm__ volatile("mf" ::: "memory");
    write32(nvme, 0x1000, 1);
    wait_irq(2, "NVMe MSI-X completion interrupt");
    volatile UINT32 *completion = (VOID *)cq;
    check((completion[3] & 0xffff) == 1 && (completion[3] >> 16) == 1,
          "NVMe completion CID/status/phase");
    check(*(UINT16 *)data == (read32(cfg, 0) & 0xffff), "NVMe IDENTIFY DMA vendor");
    unsigned stride = 4u << ((read32(nvme, 4) >> 0) & 15);
    write32(nvme, 0x1000 + stride, 1);
    write16(cfg + cap + 2, (control & ~0x8000u) | 0x4000u);
    write32(nvme, 0x14, 0);
    wait32(nvme, 0x1c, 1, 0, "NVMe final disable");
    text("NVMe MSI-X vector=99 IDENTIFY interrupt PASS\n");
}
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    system_table = st;
    EFI_GUID guid = EFI_CPU_ARCH_PROTOCOL_GUID;
    EFI_CPU_ARCH_PROTOCOL *cpu = NULL;
    check(st->BootServices->LocateProtocol(&guid, NULL, (VOID **)&cpu) == EFI_SUCCESS,
          "new firmware CPU Architectural Protocol");
    for (unsigned i = 0; i < 3; ++i)
        check(cpu->RegisterInterruptHandler(cpu, 0x61 + i, handler) == EFI_SUCCESS,
              "register guest IRQ handler");
    UINT64 lid;
    __asm__ volatile("mov %0=cr.lid" : "=r"(lid));
    device_lid = lid;
#ifdef IA64VMM_IRQ_SMP
    start_ap();
#endif
    msi_address = 0xfee00000u | ((device_lid >> 12) & 0xff000) | ((device_lid >> 12) & 0xff0);
    check(cpu->EnableInterrupt(cpu) == EFI_SUCCESS, "enable guest interrupts");
    *(volatile UINT64 *)(IA64VMM_PIB_BASE + ((lid >> 12) & 0xff000) + ((lid >> 12) & 0xff0)) = 0x61;
    __asm__ volatile("mf" ::: "memory");
    wait_irq(0, "self IPI through firmware IVT");
    text("PIB self IPI vector=97 PASS\n");
    UINT64 sum = 0, minimum = ~(UINT64)0, maximum = 0;
    for (unsigned i = 0; i < 32; ++i) {
        count[0] = 0;
        UINT64 start = itc();
        *(volatile UINT64 *)(IA64VMM_PIB_BASE + ((lid >> 12) & 0xff000) + ((lid >> 12) & 0xff0)) =
            0x61;
        __asm__ volatile("mf" ::: "memory");
        wait_irq(0, "latency self IPI");
        UINT64 ticks = ipi_delivered - start;
        sum += ticks;
        if (ticks < minimum)
            minimum = ticks;
        if (ticks > maximum)
            maximum = ticks;
    }
    diagnostic("IPI ITC ticks mean", sum / 32);
    diagnostic("IPI ITC ticks min", minimum);
    diagnostic("IPI ITC ticks max", maximum);
    ahci_test(FALSE);
    ahci_test(TRUE);
    nvme_test();
#ifdef IA64VMM_IRQ_SMP
    /* Independent IDENTIFY completions give the host INTx-hop diagnostic
     * samples; all DMA commands remain read-only on the private fixture. */
    for (unsigned i = 0; i < 31; ++i)
        ahci_test(TRUE);
    check(ap_pci_writes && ap_rte_writes, "AP1 performed PCI and RTE writes");
    diagnostic("SMP AP1 PCI writes", ap_pci_writes);
    diagnostic("SMP AP1 RTE writes", ap_rte_writes);
    stop_ap();
#endif
    for (unsigned i = 0; i < 3; ++i)
        check(cpu->RegisterInterruptHandler(cpu, 0x61 + i, NULL) == EFI_SUCCESS,
              "unregister guest IRQ handler");
    text("interrupts PASS\n");
    return EFI_SUCCESS;
}
/* GNU-EFI crt0 calls this after applying the image's ELF relocations. */
EFI_STATUS EFIAPI _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    return efi_main(image, st);
}
VOID *memcpy(VOID *destination, const VOID *source, UINTN length)
{
    UINT8 *d = destination;
    const UINT8 *s = source;
    for (UINTN i = 0; i < length; ++i)
        d[i] = s[i];
    return destination;
}
