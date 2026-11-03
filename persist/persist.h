/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VMM_PERSIST_H
#define VMM_PERSIST_H
#include "ia64vmm/bootinfo.h"
struct plog_block {
    uint64_t sectors;
    bool (*read)(uint64_t lba, unsigned sectors, void *);
    bool (*write)(uint64_t lba, unsigned sectors, const void *);
    bool (*flush)(void);
};
/* Management CPU only; no callbacks on panic. */
void plog_init(const struct boot_info *, const struct plog_block *);
void plog_poll(bool final);
void plog_failure(void);
uint64_t plog_deadline(void);
bool plog_save_ok(void);
/* "not requested", "unavailable", "refused", "verified" or "stopped" (a write failed). */
const char *plog_status(void);
bool plog_requested(const char *cmdline);

struct fat32;
struct fat32_file;
/* Locate the FAT32 partition containing the boot-loaded manifest. The
 * device may be larger than the image written to it. */
bool persist_mount(struct fat32 *, const struct plog_block *, const char *name,
                   const uint8_t manifest[512], struct fat32_file *);
/* Read-only identity presence in any bounded primary FAT32 partition. Only
 * read and sectors are required; this does not validate persistence writes. */
bool persist_identify(const struct plog_block *, const char *name, const uint8_t manifest[512]);
/* Open the preallocated files and reject cross-linked allocations. */
bool persist_files(struct fat32 *, const struct fat32_file *manifest, const char *const *names,
                   struct fat32_file *, unsigned count, uint32_t bytes);

/* A periodic duty of the reserved controller's backend that the management
 * CPU runs during guest runs, with or without media_log=1 (the EHCI port
 * watch); it returns the time of its next run, UINT64_MAX for none. */
void media_watch_set(uint64_t (*watch)(void));
uint64_t media_watch(void);

/* The host identity (struct boot_identity) from the SMBIOS entry points;
 * table maps the structure table. */
void smbios_identity(const uint8_t *entry32, const uint8_t *entry64,
                     const uint8_t *(*table)(uint64_t address, uint64_t length),
                     struct boot_identity *);

/* EFI variable slots on the VMM medium. Management
 * CPU, except nvram_stage. nvram_requested: the entry loads the nvram
 * manifest and nvram_slots=off is not given. */
bool nvram_requested(const struct boot_info *);
void nvram_init(const struct boot_info *, const struct plog_block *);
/* Before the first guest boot: the newest record of this host (its SMBIOS
 * identity) among the manifest's pairs fills the window; format 1 identifies native firmware. */
void nvram_load(unsigned format, uint8_t *window);
/* A completed update: the window is copied for the write at run end.
 * Called with the window's lock held. */
void nvram_stage(const uint8_t *window);
bool nvram_pending(void);
/* The end of the guest run: one write when the staged window changed. */
void nvram_save(void);
void nvram_identity_text(const struct boot_identity *, char *out, size_t size);
#endif
