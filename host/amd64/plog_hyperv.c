/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/amd64/plog_hyperv.h"
#include "host/amd64/hyperv.h"
#include "host/amd64/storvsc.h"
#include "core/options.h"
#include "core/log_internal.h"
#include "persist/protocol.h"
#include "platform/platform.h"
#include "platform/storage.h"
#include "ia64vmm/log.h"
#include "ia64vmm/string.h"

#if !defined(IA64VMM_AMD64_HOST_ONLY)
#define MAX_GUEST_MEDIA 32u
#define MAX_SCSI_DEVICES 512u
static const struct amd64_storvsc_device *medium, *probing, *guests[MAX_GUEST_MEDIA];
static unsigned guest_count;
static bool connected, probe_error;
static struct plog_block block;
static bool read_blocks(uint64_t at, unsigned n, void *out)
{
    return amd64_storvsc_read(medium, at, n, out);
}
static bool write_blocks(uint64_t at, unsigned n, const void *in)
{
    return amd64_storvsc_write(medium, at, n, in);
}
static bool flush(void)
{
    return amd64_storvsc_flush(medium);
}
static bool guest_read(void *ctx, uint64_t at, unsigned n, void *out)
{
    return amd64_storvsc_read(ctx, at, n, out);
}
static bool guest_write(void *ctx, uint64_t at, unsigned n, const void *in)
{
    return amd64_storvsc_write(ctx, at, n, in);
}
static bool guest_flush(void *ctx)
{
    return amd64_storvsc_flush(ctx);
}
static bool probe_read(uint64_t at, unsigned n, void *out)
{
    bool ok = amd64_storvsc_read(probing, at, n, out);
    if (!ok)
        probe_error = true;
    return ok;
}
bool amd64_hyperv_storage_start(void)
{
    if (connected)
        return true;
    if (!amd64_hyperv_detect() || !amd64_storvsc_start(NULL))
        return false;
    connected = true;
    return true;
}
static uint32_t u32(const uint8_t *p)
{
    uint32_t n;
    memcpy(&n, p, 4);
    return n;
}
static bool manifest_valid(const struct boot_module *mod, bool nvram)
{
    const uint8_t *m = mod->hva;
    if (!m || mod->size != 512 || memcmp(m, nvram ? "NVRAMMAP" : "PLOGMAP1", 8) != 0 ||
        u32(m + 8) != (nvram ? 2u : 3u) || u32(m + 12) != 512 || plog_crc(m, 508) != u32(m + 508))
        return false;
    unsigned token = 0;
    for (unsigned i = 16; i < 32; ++i)
        token |= m[i];
    if (!token)
        return false;
    if (nvram && (u32(m + 32) > 1 || !u32(m + 36) || u32(m + 36) > 32))
        return false;
    for (unsigned i = nvram ? 40 : 32; i < 508; ++i)
        if (m[i])
            return false;
    return true;
}
struct media_identity {
    const uint8_t *bytes;
    const char *file;
};
static bool identity(const struct boot_info *bi, struct media_identity *id)
{
    *id = (struct media_identity){0};
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < bi->module_count; ++i) {
            const struct boot_module *m = bi->modules + i;
            bool relevant = pass ? !strcmp(m->name, "nvram")
                                 : !strcmp(m->name, "persistence") || !strcmp(m->name, "media-id");
            if (!relevant)
                continue;
            if (!manifest_valid(m, pass != 0) || (id->bytes && memcmp(id->bytes, m->hva, 512) != 0))
                return false;
            id->bytes = m->hva;
            id->file = pass ? u32(m->hva + 32) ? "nvram-new.bin" : "nvram-ref.bin"
                            : "persistence-manifest.bin";
        }
        if (id->bytes)
            return true;
    }
    return true;
}
/* -1 means automatic; -2 is a malformed explicit selector. */
static int selector(const char *cmdline, const char *name)
{
    const char *value;
    size_t length;
    if (!platform_cmdline_value(cmdline, name, &value, &length) ||
        (length == 4 && !strncmp(value, "auto", 4)))
        return -1;
    int lun = options_hyperv_lun(value, length);
    return lun < 0 ? -2 : lun;
}
static bool matches_identity(const struct amd64_storvsc_device *d, const struct media_identity *id)
{
    probe_error = false;
    if (!id->bytes || d->peripheral_type != 0 || d->sector_bytes != 512)
        return false;
    probing = d;
    const struct plog_block probe = {d->blocks, probe_read, NULL, NULL};
    return persist_identify(&probe, id->file, id->bytes);
}
static bool guest_supported(const struct amd64_storvsc_device *d)
{
    return d->blocks &&
           ((d->peripheral_type == 0 && (d->sector_bytes == 512 || d->sector_bytes == 4096) &&
             d->blocks <= (UINT64_C(1) << 48)) ||
            (d->peripheral_type == 5 && d->sector_bytes == 2048 && d->readonly &&
             d->blocks <= (UINT64_C(1) << 32)));
}
const struct plog_block *amd64_plog_hyperv(const struct boot_info *bi)
{
    if (!bi || bi->module_count > BOOT_MAX_MODULES)
        return NULL;
    int vmm_lun = selector(bi->cmdline, "vmm_media"),
        guest_lun = selector(bi->cmdline, "guest_media");
    struct media_identity id;
    if (vmm_lun == -2 || guest_lun == -2 || !identity(bi, &id) || (vmm_lun < 0 && !id.bytes)) {
        LOGE("Hyper-V media selector or boot-loaded identity is invalid/missing");
        return NULL;
    }
    if (!amd64_hyperv_storage_start() || !amd64_storvsc_enumerate())
        return NULL;
    const struct amd64_storvsc_device *selected =
        vmm_lun >= 0 ? amd64_storvsc_find(0, (unsigned)vmm_lun) : NULL;
    const struct amd64_storvsc_device *selected_guest =
        guest_lun >= 0 ? amd64_storvsc_find(0, (unsigned)guest_lun) : NULL;
    if ((vmm_lun >= 0 && !selected) || (guest_lun >= 0 && !selected_guest))
        return NULL;
    bool matching[MAX_SCSI_DEVICES] = {0}, selected_matches = false;
    unsigned count = amd64_storvsc_count(), matches = 0;
    if (count > MAX_SCSI_DEVICES)
        return NULL;
    for (unsigned i = 0; i < count; ++i) {
        const struct amd64_storvsc_device *d = amd64_storvsc_device(i);
        if (!d)
            return NULL;
        matching[i] = matches_identity(d, &id);
        if (probe_error) {
            LOGE("Hyper-V media identity scan encountered an I/O error");
            return NULL;
        }
        if (!matching[i])
            continue;
        ++matches;
        if (vmm_lun < 0)
            selected = d;
        if (d == selected)
            selected_matches = true;
    }
    if ((vmm_lun < 0 && matches != 1) || (id.bytes && !selected_matches) || !selected ||
        selected->peripheral_type != 0 || selected->sector_bytes != 512 || selected->readonly) {
        LOGE("Hyper-V VMM medium requires one matching writable 512-byte disk (matches=%u)",
             matches);
        return NULL;
    }
    medium = selected;
    guest_count = 0;
    for (unsigned i = 0; i < count; ++i) {
        const struct amd64_storvsc_device *d = amd64_storvsc_device(i);
        if (selected_guest && d != selected_guest)
            continue;
        if (d == medium || matching[i] || !guest_supported(d)) {
            if (selected_guest) {
                LOGE("Hyper-V guest medium is reserved or has an unsupported format");
                return NULL;
            }
            continue;
        }
        if (guest_count == MAX_GUEST_MEDIA) {
            LOGE("Hyper-V guest media exceeds AHCI port capacity");
            return NULL;
        }
        unsigned at = guest_count++;
        while (at && (guests[at - 1]->target > d->target ||
                      (guests[at - 1]->target == d->target && guests[at - 1]->lun > d->lun))) {
            guests[at] = guests[at - 1];
            --at;
        }
        guests[at] = d;
    }
    for (unsigned i = 0; i < guest_count; ++i) {
        const struct amd64_storvsc_device *d = guests[i];
        const struct platform_block backing = {.context = (void *)d,
                                               .sectors = d->blocks,
                                               .sector_bytes = d->sector_bytes,
                                               .read_only = d->readonly,
                                               .read = guest_read,
                                               .write = guest_write,
                                               .flush = guest_flush};
        if (!platform_storage_attach(&backing)) {
            (void)platform_storage_attach(NULL);
            guest_count = 0;
            LOGE("Hyper-V guest storage resources conflict with another device");
            return NULL;
        }
        LOGI("Hyper-V guest port=%u target=%u LUN=%u sectors=%llu sector_bytes=%u readonly=%u", i,
             d->target, d->lun, (unsigned long long)d->blocks, d->sector_bytes, d->readonly);
    }
    block = (struct plog_block){medium->blocks, read_blocks, write_blocks, flush};
    LOGI("Hyper-V VMM media: target=%u LUN=%u sectors=%llu identity=%s", medium->target,
         medium->lun, (unsigned long long)medium->blocks,
         id.bytes ? "verified" : "explicit selection");
    return &block;
}
bool amd64_hyperv_storage_discover(void)
{
    if (!amd64_hyperv_storage_start() || !amd64_storvsc_enumerate())
        return false;
    for (unsigned i = 0; i < amd64_storvsc_count(); ++i) {
        const struct amd64_storvsc_device *d = amd64_storvsc_device(i);
        LOGI("Hyper-V SCSI target=%u LUN=%u sectors=%llu sector_bytes=%u readonly=%u", d->target,
             d->lun, (unsigned long long)d->blocks, d->sector_bytes, d->readonly);
    }
    LOGI("Hyper-V media selection is automatic; optional overrides use hyperv:LUN");
    return true;
}
void amd64_plog_hyperv_shutdown(void)
{
    if (!connected || log_in_panic())
        return;
    for (unsigned i = 0; i < guest_count; ++i)
        if (!guests[i]->readonly && !amd64_storvsc_flush(guests[i]))
            LOGE("Hyper-V guest media flush failed");
    amd64_storvsc_stop();
    connected = false;
}
#endif
