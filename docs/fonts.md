<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Console font

`host/fb/font.h` contains public-domain X.Org misc-fixed `9x15.bdf`
glyphs: "Public domain font. Share and enjoy."

Regenerate the table with `python3 tools/fonts/mkfont.py 9x15.bdf output.h`.
The conversion crops the ninth column, aligns the baseline to row 12 in a
16-row cell, and retains ASCII 0x20–0x7e and a replacement glyph.
The generator is GPL-2.0-or-later; the glyphs remain public domain.
