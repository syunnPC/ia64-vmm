#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline parser for PCI expansion ROM images and GPU flash dumps.

Reports every image of a ROM chain (PCI Firmware Specification 3.0 §5.1),
its PCI Data Structure, the code type (0 = x86 PC-AT legacy, 3 = EFI,
0xE0 = NVIDIA-specific), and the parts a real-mode interpreter needs from a
legacy image: initialisation size, entry jump chain, checksum, $PnP header
and VBE 3.0 PMID block. Vendor markers (NVIDIA BIT, Intel $VBT, AMD ATOM)
are reported, not interpreted.

Input is untrusted data: every offset is bounds-checked and the tool never
executes or writes the ROM. A full flash dump (GPU-Z "Save BIOS", nvflash
--save) may carry a vendor header before the chain, so image candidates are
searched at every 512-byte boundary (PCI FW 3.0 §5.1.1 aligns images to
512 bytes) and chains are followed from each unclaimed candidate.

NVIDIA chains continue past the PCI "last image" indicator: the NVIDIA PCI
Data Extension (NPDE) that follows the PCI Data Structure carries its own
sub-image length and last-image flag, and NVIDIA images may use the 'VN'
or 0xBB77 image signature and the 'NPDS'/'RGIS' data-structure signature.
These are the signatures an NVIDIA legacy image's own chain walker accepts.
"""

import argparse
import json
from pathlib import Path
import struct
import sys

IMAGE_SIGNATURES = {b"\x55\xaa": "55AA", b"VN": "VN", b"\x77\xbb": "BB77"}
DATA_SIGNATURES = {b"PCIR": "PCIR", b"NPDS": "NPDS", b"RGIS": "RGIS"}
CODE_TYPES = {
    0x00: "x86 PC-AT (legacy BIOS)",
    0x01: "Open Firmware",
    0x02: "HP PA-RISC",
    0x03: "EFI",
    0xE0: "NVIDIA-specific",
}
EFI_SUBSYSTEMS = {10: "application", 11: "boot service driver", 12: "runtime driver"}
EFI_MACHINES = {
    0x014C: "IA32",
    0x0200: "IA64",
    0x0EBC: "EBC",
    0x8664: "X64",
    0xAA64: "AARCH64",
    0x5064: "RISCV64",
    0x6264: "LOONGARCH64",
}
MAX_JUMPS = 8


def u8(b, o):
    return b[o] if 0 <= o < len(b) else None


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0] if 0 <= o and o + 2 <= len(b) else None


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0] if 0 <= o and o + 4 <= len(b) else None


def ascii_at(b, o, limit=64):
    """NUL- or non-printable-terminated ASCII at o, at most limit bytes."""
    out = []
    for c in b[o : o + limit]:
        if c < 0x20 or c > 0x7E:
            break
        out.append(chr(c))
    return "".join(out)


def data_structure(rom, image_off):
    """PCI Data Structure (PCI FW 3.0 §5.1.2) or an NVIDIA equivalent."""
    ptr = u16(rom, image_off + 0x18)
    if ptr is None:
        return None, "image header truncated before the data-structure pointer"
    at = image_off + ptr
    sig = bytes(rom[at : at + 4])
    if sig not in DATA_SIGNATURES:
        return None, "no PCI data structure at +0x%x" % ptr
    if at + 0x18 > len(rom):
        return None, "PCI data structure at +0x%x truncated" % ptr
    vendor, device, devlist, length, revision = struct.unpack_from("<HHHHB", rom, at + 4)
    cls = rom[at + 0x0D] | rom[at + 0x0E] << 8 | rom[at + 0x0F] << 16
    image_len, code_rev, code_type, indicator = struct.unpack_from("<HHBB", rom, at + 0x10)
    ds = {
        "offset": ptr,
        "signature": DATA_SIGNATURES[sig],
        "vendor": vendor,
        "device": device,
        "device_list": devlist,
        "length": length,
        "revision": revision,
        "class_code": cls,
        "image_length": image_len * 512,
        "code_revision": code_rev,
        "code_type": code_type,
        "code_type_name": CODE_TYPES.get(code_type, "unknown"),
        "last": bool(indicator & 0x80),
    }
    # PCI 3.0 (revision 3, length >= 0x1C) adds the runtime/config/CLP fields.
    if revision >= 3 and length >= 0x1C and at + 0x1C <= len(rom):
        ds["max_runtime_length"] = u16(rom, at + 0x16) * 512
        ds["config_utility"] = u16(rom, at + 0x18)
        ds["dmtf_clp"] = u16(rom, at + 0x1A)
    # The NVIDIA PCI Data Extension follows the structure, 16-byte aligned.
    npde_at = (at + max(length, 0x18) + 15) & ~15
    if bytes(rom[npde_at : npde_at + 4]) == b"NPDE" and npde_at + 0x0C <= len(rom):
        rev, hlen, sub_len = struct.unpack_from("<HHH", rom, npde_at + 4)
        ds["npde"] = {
            "offset": npde_at - image_off,
            "revision": rev,
            "length": hlen,
            "subimage_length": sub_len * 512,
            "last": bool(rom[npde_at + 0x0A] & 0x80),
            "flags": rom[npde_at + 0x0B],
        }
    return ds, None


def jump_chain(rom, base, size):
    """Follow unconditional near/short/far jumps from the init entry (offset 3)."""
    chain, at = [3], 3
    for _ in range(MAX_JUMPS):
        op = u8(rom, base + at)
        if op == 0xEB and at + 2 <= size:
            nxt = at + 2 + struct.unpack_from("<b", rom, base + at + 1)[0]
        elif op == 0xE9 and at + 3 <= size:
            nxt = (at + 3 + struct.unpack_from("<h", rom, base + at + 1)[0]) & 0xFFFF
        elif op == 0xEA and at + 5 <= size:
            chain.append("far %04x:%04x" % (u16(rom, base + at + 3), u16(rom, base + at + 1)))
            break
        else:
            break
        if not 0 <= nxt < size or nxt in chain:
            chain.append("outside image: 0x%x" % nxt)
            break
        chain.append(nxt)
        at = nxt
    return chain


def pnp_header(rom, base, size):
    """$PnP expansion header (PCI FW 3.0 §5.2.2; BIOS Boot Spec App. A)."""
    ptr = u16(rom, base + 0x1A)
    if not ptr:
        return {"present": False, "pointer": ptr}
    at = base + ptr
    if ptr + 0x20 > size or bytes(rom[at : at + 4]) != b"$PnP":
        return {
            "present": False,
            "pointer": ptr,
            "note": "offset 0x1A does not point to a $PnP header",
        }
    length = rom[at + 5] * 16
    info = {
        "present": True,
        "pointer": ptr,
        "revision": rom[at + 4],
        "length": length,
        "next": u16(rom, at + 6),
        "checksum_ok": ptr + length <= size and sum(rom[at : at + length]) & 0xFF == 0,
        "device_id": u32(rom, at + 0x0A),
        "device_type": rom[at + 0x12] | rom[at + 0x13] << 8 | rom[at + 0x14] << 16,
        "indicators": rom[at + 0x15],
        "bcv": u16(rom, at + 0x16),
        "dv": u16(rom, at + 0x18),
        "bev": u16(rom, at + 0x1A),
    }
    for key, off in (("manufacturer", 0x0E), ("product", 0x10)):
        p = u16(rom, at + off)
        if p and p < size:
            info[key] = ascii_at(rom, base + p)
    return info


def find_all(rom, base, size, needle):
    out, at = [], rom.find(needle, base, base + size)
    while at >= 0:
        out.append(at - base)
        at = rom.find(needle, at + 1, base + size)
    return out


def legacy_details(rom, base, size):
    init = rom[base + 2] * 512
    region = min(init, size, len(rom) - base)
    d = {
        "init_size": init,
        "entry_chain": jump_chain(rom, base, size),
        "checksum_ok": init > 0 and region == init and sum(rom[base : base + init]) & 0xFF == 0,
        "pnp": pnp_header(rom, base, size),
    }
    if bytes(rom[base + 0x1E : base + 0x21]) == b"IBM":
        d["ibm_string"] = ascii_at(rom, base + 0x1E, 32)
    pmids = []
    for off in find_all(rom, base, size, b"PMID"):
        if off + 20 <= size:
            entry, init_pm, data_sel, a000, b000, b800, code_sel, inpm, _ = struct.unpack_from(
                "<HHHHHHHBB", rom, base + off + 4
            )
            pmids.append(
                {
                    "offset": off,
                    "entry": entry,
                    "init": init_pm,
                    "segments": [data_sel, a000, b000, b800, code_sel],
                    "checksum_ok": sum(rom[base + off : base + off + 20]) & 0xFF == 0,
                }
            )
    # VBE 3.0 §5.4: PMID is 20 bytes with a zero byte sum; keep valid ones.
    d["vbe_pmid"] = [p for p in pmids if p["checksum_ok"]]
    return d


def efi_details(rom, base, size):
    d = {
        "init_size": (u16(rom, base + 2) or 0) * 512,
        "efi_signature_ok": u32(rom, base + 4) == 0x0EF1,
        "subsystem": u16(rom, base + 8),
        "machine": u16(rom, base + 0x0A),
        "compression": u16(rom, base + 0x0C),
        "image_offset": u16(rom, base + 0x16),
    }
    d["subsystem_name"] = EFI_SUBSYSTEMS.get(d["subsystem"], "unknown")
    d["machine_name"] = EFI_MACHINES.get(d["machine"], "unknown")
    io = d["image_offset"]
    if io is not None and d["compression"] == 0 and io + 2 <= size:
        d["pe_header"] = bytes(rom[base + io : base + io + 2]) == b"MZ"
    return d


def vendor_markers(rom, base, size):
    """Signatures that identify the vendor's own tables; offsets are image-relative."""
    m = {}
    bit = find_all(rom, base, size, b"\xff\xb8BIT\x00")
    if bit:
        at = base + bit[0]
        m["nvidia_bit"] = {
            "offset": bit[0],
            "version": u16(rom, at + 6),
            "header_size": u8(rom, at + 8),
            "token_size": u8(rom, at + 9),
            "token_count": u8(rom, at + 10),
        }
    vbt = find_all(rom, base, size, b"$VBT")
    if vbt:
        m["intel_vbt"] = vbt[0]
    if bytes(rom[base + 0x30 : base + 0x3A]) == b" 761295520":
        hdr = u16(rom, base + 0x48)
        m["amd_atom"] = {
            "magic_ok": True,
            "rom_header": hdr,
            "atom_signature": hdr is not None
            and hdr + 8 <= size
            and bytes(rom[base + hdr + 4 : base + hdr + 8]) == b"ATOM",
        }
    return m


def image_at(rom, off):
    """Decode one image candidate at file offset off, or return None."""
    sig = IMAGE_SIGNATURES.get(bytes(rom[off : off + 2]))
    if sig is None:
        return None
    ds, err = data_structure(rom, off)
    if ds is None:
        return None
    img = {"offset": off, "signature": sig, "pcir": ds, "warnings": []}
    length = ds["npde"]["subimage_length"] if "npde" in ds else ds["image_length"]
    img["length"] = length
    if length == 0:
        img["warnings"].append("zero image length")
        return img
    if off + length > len(rom):
        img["warnings"].append(
            "image extends 0x%x bytes past the end of the file" % (off + length - len(rom))
        )
    if "npde" in ds and ds["npde"]["subimage_length"] != ds["image_length"]:
        img["warnings"].append(
            "NPDE sub-image length 0x%x differs from PCIR length 0x%x"
            % (ds["npde"]["subimage_length"], ds["image_length"])
        )
    size = min(length, len(rom) - off)
    if ds["code_type"] == 0x00:
        img["legacy"] = legacy_details(rom, off, size)
        if not img["legacy"]["checksum_ok"]:
            img["warnings"].append("legacy initialisation area checksum is not zero")
    elif ds["code_type"] == 0x03:
        img["efi"] = efi_details(rom, off, size)
        if not img["efi"]["efi_signature_ok"]:
            img["warnings"].append("EFI image without the 0x0EF1 signature")
    markers = vendor_markers(rom, off, size)
    if markers:
        img["markers"] = markers
    return img


def follow_chain(rom, start):
    """Walk images from start: PCI indicator first, then NPDE continuation."""
    images, off, pci_end = [], start, None
    while off is not None and off < len(rom):
        img = image_at(rom, off)
        if img is None:
            break
        images.append(img)
        ds = img["pcir"]
        if pci_end is None and ds["last"]:
            pci_end = off + img["length"]
        npde_last = ds.get("npde", {}).get("last")
        last = npde_last if npde_last is not None else ds["last"]
        if last or img["length"] == 0 or off + img["length"] > len(rom):
            break
        off += img["length"]
    return images, pci_end


def candidates(rom):
    return [o for o in range(0, len(rom) - 0x1A, 512) if image_at(rom, o) is not None]


def parse(rom, start=None):
    rom = bytes(rom)
    found = candidates(rom)
    report = {"size": len(rom), "chains": []}
    if start is not None:
        found = [start] + [o for o in found if o != start]
    claimed = set()
    for off in found:
        if off in claimed:
            continue
        images, pci_end = follow_chain(rom, off)
        if not images:
            continue
        claimed.update(i["offset"] for i in images)
        chain = {
            "start": off,
            "images": images,
            "pci_rom_end": pci_end,
            "end": images[-1]["offset"] + images[-1]["length"],
        }
        report["chains"].append(chain)
    report["legacy_images"] = [
        i["offset"] for c in report["chains"] for i in c["images"] if i["pcir"]["code_type"] == 0
    ]
    return report


def describe(report):
    lines = ["file size 0x%x (%d bytes)" % (report["size"], report["size"])]
    if not report["chains"]:
        lines.append("no PCI expansion ROM image found")
    for n, chain in enumerate(report["chains"]):
        lines.append(
            "chain %d: start 0x%x, end 0x%x%s"
            % (
                n,
                chain["start"],
                chain["end"],
                ", PCI last-image indicator ends at 0x%x" % chain["pci_rom_end"]
                if chain["pci_rom_end"] is not None
                else "",
            )
        )
        for img in chain["images"]:
            ds = img["pcir"]
            line = "  0x%06x %s/%s %04x:%04x class %06x code type 0x%02x (%s) length 0x%x%s" % (
                img["offset"],
                img["signature"],
                ds["signature"],
                ds["vendor"],
                ds["device"],
                ds["class_code"],
                ds["code_type"],
                ds["code_type_name"],
                img["length"],
                " PCI-last" if ds["last"] else "",
            )
            if "npde" in ds:
                line += " NPDE(rev 0x%x, sub-image 0x%x%s)" % (
                    ds["npde"]["revision"],
                    ds["npde"]["subimage_length"],
                    ", last" if ds["npde"]["last"] else "",
                )
            lines.append(line)
            if "legacy" in img:
                lg = img["legacy"]
                entry = " -> ".join(
                    e if isinstance(e, str) else "0x%x" % e for e in lg["entry_chain"]
                )
                lines.append(
                    "    legacy: init size 0x%x, checksum %s, entry %s"
                    % (lg["init_size"], "ok" if lg["checksum_ok"] else "BAD", entry)
                )
                pnp = lg["pnp"]
                lines.append(
                    "    $PnP header: %s"
                    % (
                        "present, rev %d, checksum %s"
                        % (pnp["revision"], "ok" if pnp["checksum_ok"] else "BAD")
                        if pnp["present"]
                        else "absent" + (" (%s)" % pnp["note"] if "note" in pnp else "")
                    )
                )
                for p in lg["vbe_pmid"]:
                    lines.append(
                        "    VBE 3.0 PMID at 0x%x: entry 0x%x init 0x%x"
                        % (p["offset"], p["entry"], p["init"])
                    )
                if "ibm_string" in lg:
                    lines.append('    0x1E: "%s"' % lg["ibm_string"])
            if "efi" in img:
                e = img["efi"]
                lines.append(
                    "    EFI: subsystem %s (%s), machine 0x%x (%s), compression %s%s"
                    % (
                        e["subsystem"],
                        e["subsystem_name"],
                        e["machine"],
                        e["machine_name"],
                        e["compression"],
                        ", PE image present" if e.get("pe_header") else "",
                    )
                )
            for key, val in img.get("markers", {}).items():
                lines.append("    marker %s: %s" % (key, val))
            for w in img["warnings"]:
                lines.append("    warning: %s" % w)
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("rom", type=Path)
    ap.add_argument("--start", type=lambda s: int(s, 0), help="force a chain start offset")
    ap.add_argument("--json", action="store_true", help="print the report as JSON")
    ap.add_argument(
        "--extract-legacy",
        type=Path,
        metavar="OUT",
        help="write the first code type 0 image to OUT (for an offline interpreter)",
    )
    args = ap.parse_args(argv)
    rom = args.rom.read_bytes()
    report = parse(rom, args.start)
    print(json.dumps(report, indent=2) if args.json else describe(report))
    if args.extract_legacy:
        legacy = [i for c in report["chains"] for i in c["images"] if i["pcir"]["code_type"] == 0]
        if not legacy:
            print("no code type 0 image to extract", file=sys.stderr)
            return 3
        img = legacy[0]
        args.extract_legacy.write_bytes(rom[img["offset"] : img["offset"] + img["length"]])
    return 0 if report["chains"] else 2


if __name__ == "__main__":
    sys.exit(main())
