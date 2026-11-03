/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Direct DXE IPL. Reuses EDK2 BasePeCoffLib and BaseLib's IPF stack switch
 * rather than introducing another PE relocator or another RSE convention.
 */
#include "hob.h"
#include "platform.h"
#include "unaligned.h"
#include "ap.h"
#include "ivt-layout.h"
#include "boot-translation.h"
#include "sal/mc.h"
#include <Pi/PiFirmwareVolume.h>
#include <Pi/PiFirmwareFile.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PeCoffLib.h>
#include <Library/DebugLib.h>

extern const UINT8 dxe_fv[], dxe_fv_end[], ivt_base[];
extern struct ap_wake_block ap_wake_block;
extern struct sal_mc sal_mc;
void sal_cpu_init(void);
extern VOID EFIAPI AsmSwitchStackAndBackingStore(VOID *entry, VOID *context1, VOID *context2,
                                                 VOID *stack, VOID *bsp);
/* Match the frozen MdeModulePkg fixed PCD configuration for IPL libraries. */
const UINT32 _gPcd_FixedAtBuild_PcdMaximumLinkedListLength = 1000000;
const UINT32 _gPcd_FixedAtBuild_PcdMaximumAsciiStringLength = 1000000;
const UINT32 _gPcd_FixedAtBuild_PcdMaximumUnicodeStringLength = 1000000;
const struct ia64vmm_handoff *unaligned_handoff;

struct image_source {
    const UINT8 *bytes;
    UINTN size;
};
static RETURN_STATUS EFIAPI read_image(VOID *handle, UINTN offset, UINTN *size, VOID *buffer)
{
    struct image_source *source = handle;
    if (offset > source->size || *size > source->size - offset)
        return RETURN_DEVICE_ERROR;
    CopyMem(buffer, source->bytes + offset, *size);
    return RETURN_SUCCESS;
}

static VOID fail(const CHAR8 *reason)
{
    DebugPrint(DEBUG_ERROR, "IPL failure: %a\n", reason);
    *(volatile UINT64 *)(IA64VMM_SERVICE_BASE + IA64VMM_SVC_PANIC) = 0x47314950;
    for (;;)
        __asm__ volatile("hint @pause");
}

static UINT32 size24(const UINT8 *bytes)
{
    return (UINT32)bytes[0] | (UINT32)bytes[1] << 8 | (UINT32)bytes[2] << 16;
}

/* HOBs, the 8 KiB control block (ZeroMem below) and the BSP
 * interruption stack are disjoint and below the DXE core. */
_Static_assert(IPL_HOB_OFFSET + IPL_HOB_CAPACITY <= PLATFORM_CONTROL_OFFSET &&
                   PLATFORM_CONTROL_OFFSET + 0x2000 <= IPL_INTERRUPT_STACK_OFFSET &&
                   IPL_INTERRUPT_STACK_OFFSET + IPL_INTERRUPT_STACK_SIZE <= IPL_CORE_OFFSET &&
                   IPL_INTERRUPT_STACK_SIZE % 16 == 0,
               "fw_data layout");
_Static_assert(SAL_MIN_STATE_CPUS == IA64VMM_VCPU_LIMIT,
               "one min-state area per vCPU (sal/entry.S)");

VOID ipl_main(const struct ia64vmm_handoff *handoff, UINTN size, UINTN id)
{
    const UINT8 *fv_start, *fv_end, *ivt;
    /* The embedded FV can exceed GP's 22-bit short-data reach. Absolute
     * movl references use the loader's validated IMM64 relocation contract. */
    __asm__ volatile("movl %0=dxe_fv;; movl %1=dxe_fv_end;; movl %2=ivt_base;;"
                     : "=r"(fv_start), "=r"(fv_end), "=r"(ivt));
    if (!ipl_validate_handoff(handoff, size, id))
        fail("invalid handoff");
    static struct ia64vmm_handoff normalized;
    ia64vmm_handoff_copy(&normalized, handoff);
    handoff = &normalized;
    UINT64 *pal_count;
    __asm__ volatile("movl %0=pal_cpu_count;;" : "=r"(pal_count));
    *pal_count = handoff->cpu_count;
    UINT64 *control = (VOID *)(UINTN)(handoff->fw_data_base + PLATFORM_CONTROL_OFFSET);
    ZeroMem(control, 0x2000);
    UINT64 pal_entry;
    __asm__ volatile("movl %0=pal_proc_entry;;" : "=r"(pal_entry));
    control[5656 / 8] = pal_entry;
    unaligned_handoff = handoff;
    const UINT64 *unaligned_descriptor = (const VOID *)unaligned_dispatch;
    control[5688 / 8] = unaligned_descriptor[0];
    control[5696 / 8] = unaligned_descriptor[1];
    control[CONTROL_ApBlock / 8] = (UINTN)&ap_wake_block;
    control[CONTROL_SalMc / 8] = (UINTN)&sal_mc;
    control[CONTROL_InterruptStack / 8] =
        handoff->fw_data_base + IPL_INTERRUPT_STACK_OFFSET + IPL_INTERRUPT_STACK_SIZE - 16;
    __asm__ volatile("mov ar.k1=%0;;" ::"r"(control) : "memory");
    struct ipl_boot_translation tr;
    if (!ipl_install_boot_translation(&tr))
        fail("PAL and IVT do not fit ITR(0)");
    control[CONTROL_BootItrAddress / 8] = tr.address;
    control[CONTROL_BootItrItir / 8] = tr.itir;
    __asm__ volatile("mov cr.iva=%0;; srlz.i;; ssm psr.ic;; srlz.d;;" ::"r"(ivt) : "memory");
    /* SAL §4.1.1, §5.1, §6.2: min-state area and SAL PMI entrypoint. */
    sal_cpu_init();
    DebugPrint(DEBUG_INFO, "IPL: validated handoff\n");
    UINTN fv_size = fv_end - fv_start;
    const EFI_FIRMWARE_VOLUME_HEADER *fv = (const VOID *)fv_start;
    if (fv_size < sizeof(*fv) || fv->Signature != EFI_FVH_SIGNATURE || fv->FvLength != fv_size ||
        fv->HeaderLength < sizeof(*fv) || fv->HeaderLength > fv_size)
        fail("bad FV header");
    const EFI_FFS_FILE_HEADER *file = (const VOID *)(fv_start + fv->HeaderLength);
    if (fv_size - fv->HeaderLength < sizeof(*file))
        fail("truncated DXE Core file");
    UINTN file_size = size24(file->Size);
    if (file->Type != EFI_FV_FILETYPE_DXE_CORE || file_size < sizeof(*file) + 4 ||
        file_size > fv_size - fv->HeaderLength)
        fail("bad DXE Core file");
    const EFI_COMMON_SECTION_HEADER *section = (const VOID *)(file + 1);
    UINTN section_size = size24(section->Size);
    if (section->Type != EFI_SECTION_PE32 || section_size < sizeof(*section) ||
        section_size > file_size - sizeof(*file))
        fail("bad DXE Core PE section");
    struct image_source source = {(const UINT8 *)(section + 1), section_size - sizeof(*section)};
    PE_COFF_LOADER_IMAGE_CONTEXT image;
    ZeroMem(&image, sizeof(image));
    image.Handle = &source;
    image.ImageRead = read_image;
    if (RETURN_ERROR(PeCoffLoaderGetImageInfo(&image)))
        fail("PE image info");
    if (image.Machine != 0x200 || image.ImageSize > IPL_BSP_OFFSET - IPL_CORE_OFFSET ||
        image.SectionAlignment != 4096 || image.RelocationsStripped)
        fail("PE image layout");
    image.ImageAddress = handoff->fw_data_base + IPL_CORE_OFFSET;
    ZeroMem((VOID *)(UINTN)image.ImageAddress, (UINTN)image.ImageSize);
    if (RETURN_ERROR(PeCoffLoaderLoadImage(&image)) ||
        RETURN_ERROR(PeCoffLoaderRelocateImage(&image)))
        fail("PE load/relocate");
    /* The VMM invalidates translations on writes. fc/sync.i also makes this
     * correct for an IA-64 implementation with separate instruction caches.
     */
    for (UINTN at = (UINTN)image.ImageAddress; at < image.ImageAddress + image.ImageSize; at += 32)
        __asm__ volatile("fc %0" ::"r"(at) : "memory");
    __asm__ volatile("sync.i;; srlz.i;;" ::: "memory");
    VOID *hobs = ipl_build_hobs(handoff, (VOID *)(UINTN)(handoff->fw_data_base + IPL_HOB_OFFSET),
                                IPL_HOB_CAPACITY, image.ImageAddress, image.ImageSize,
                                image.EntryPoint, (UINTN)fv_start, fv_size);
    if (!hobs)
        fail("HOB construction");
    DebugPrint(DEBUG_INFO, "IPL: entering DxeCore descriptor=%lx hobs=%lx\n", image.EntryPoint,
               (UINTN)hobs);
    AsmSwitchStackAndBackingStore(
        (VOID *)(UINTN)image.EntryPoint, hobs, NULL,
        (VOID *)(UINTN)(handoff->fw_data_base + IPL_STACK_OFFSET + IPL_STACK_SIZE - 16),
        (VOID *)(UINTN)(handoff->fw_data_base + IPL_BSP_OFFSET));
    fail("DxeCore returned");
}
