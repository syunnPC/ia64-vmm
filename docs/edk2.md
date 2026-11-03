<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# EDK2 sources

The selected Tianocore EDK2 sources in `third_party/edk2-ipf` come from
vUDK2018, commit `3e72ffe8afdd03f1f89eba65c921cbdcb004cfee`.
They retain their original BSD-2-Clause notices. The bundled Brotli licence
is MIT; no Brotli implementation is compiled.

The import includes the IA-64 DXE core, runtime services, PCI, storage,
USB, keyboard, console, HII, variable and filesystem drivers and required
libraries. UDF and OHCI drivers are not included.

`selection.txt` records the source selection. `firmware/edk2/manifest.json` identifies
compiled sources and libraries. Project overlays and generated build
files live under `firmware/edk2` and `firmware/new`; patches apply to build
copies and preserve the imported source bytes.

Regeneration
requires the pinned upstream checkout in `EDK2_IPF_DIR`, Python 2.7 in
`EDK2_PYTHON2`, and `python3 tools/firmware/regenerate_edk2.py`.
