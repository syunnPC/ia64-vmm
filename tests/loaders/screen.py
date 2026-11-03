# SPDX-License-Identifier: GPL-2.0-or-later
"""Read PNG pixels and the firmware's 80x25 glyph grid for menu acceptance."""

from collections import Counter
from pathlib import Path
import re
import struct
import zlib


def rgb_pixels(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    at, header, payload = 8, None, bytearray()
    while at < len(data):
        if len(data) - at < 12:
            raise ValueError("truncated PNG chunk")
        size = struct.unpack_from(">I", data, at)[0]
        if size > len(data) - at - 12:
            raise ValueError("truncated PNG chunk body")
        kind, body = data[at + 4 : at + 8], data[at + 8 : at + 8 + size]
        if kind == b"IHDR":
            if size != 13 or header is not None:
                raise ValueError("invalid PNG header")
            header = struct.unpack(">IIBBBBB", body)
        if kind == b"IDAT":
            payload.extend(body)
        at += size + 12
        if kind == b"IEND":
            break
    if header is None:
        raise ValueError("missing PNG header")
    width, height, depth, color, compression, filtering, interlace = header
    if (
        depth != 8
        or color not in (2, 6)
        or compression
        or filtering
        or interlace
        or not 80 <= width <= 4096
        or not 25 <= height <= 4096
    ):
        raise ValueError("unsupported PNG pixel format or dimensions")
    channels = 3 if color == 2 else 4
    stride = width * channels
    raw = zlib.decompress(payload)
    if len(raw) != (stride + 1) * height:
        raise ValueError("invalid PNG raster length")
    previous, pixels, at = bytearray(stride), [], 0
    for _ in range(height):
        mode = raw[at]
        row = bytearray(raw[at + 1 : at + 1 + stride])
        at += stride + 1
        if mode > 4:
            raise ValueError("invalid PNG filter")
        if mode:
            for i in range(stride):
                a = row[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                if mode == 1:
                    add = a
                elif mode == 2:
                    add = b
                elif mode == 3:
                    add = (a + b) // 2
                else:
                    p = a + b - c
                    distances = [abs(p - a), abs(p - b), abs(p - c)]
                    add = [a, b, c][distances.index(min(distances))]
                row[i] = (row[i] + add) & 255
        pixels.append([bytes(row[x * channels : x * channels + 3]) for x in range(width)])
        previous = row
    return width, height, pixels


def firmware_font(path):
    source = Path(path).read_text().split("{", 1)[1]
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    font = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})\b", source))
    if len(font) != 4096:
        raise ValueError("unexpected firmware font")
    return font


def firmware_text(path, font_path):
    width, height, pixels = rgb_pixels(path)
    font = firmware_font(font_path)
    glyphs = {font[c * 16 : (c + 1) * 16]: chr(c) for c in range(32, 127)}
    lines = []
    for row in range(25):
        text = ""
        for col in range(80):
            x0, x1 = col * width // 80, (col + 1) * width // 80
            y0, y1 = row * height // 25, (row + 1) * height // 25

            # The renderer repeats source pixels with floor(dst*n/extent).
            # Sample the inverse intervals, including fractional GOP scaling.
            def sample(g, extent, n):
                start = (g * extent + n - 1) // n
                end = ((g + 1) * extent + n - 1) // n
                return min(extent - 1, (start + end - 1) // 2)

            samples = [
                [
                    pixels[y0 + sample(gy, y1 - y0, 16)][x0 + sample(gx, x1 - x0, 8)]
                    for gx in range(8)
                ]
                for gy in range(16)
            ]
            background = Counter(p for line in samples for p in line).most_common(1)[0][0]
            bits = bytes(
                sum((p != background) << (7 - i) for i, p in enumerate(line)) for line in samples
            )
            text += glyphs.get(bits, "?")
        lines.append(text.rstrip())
    return "\n".join(lines)


def menu_matches(path, words, font_path):
    try:
        text = firmware_text(path, font_path)
        normalized = " ".join(text.split())
        return all(" ".join(word.split()) in normalized for word in words), text
    except (OSError, ValueError, IndexError, struct.error, zlib.error) as error:
        return False, str(error)


def edk2_menu_matches(path, words, font_path):
    """Recognize UDK2018's unscaled 8x19 centered GraphicsConsole modes."""
    try:
        source = Path(font_path).read_text()
        glyphs = {}
        for char, rows in re.findall(
            r"\{\s*0x([0-9a-fA-F]+),\s*0x[0-9a-fA-F]+,\s*\{([^}]+)\}", source
        ):
            character = int(char, 16)
            bitmap = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})\b", rows))
            if 32 <= character < 127 and len(bitmap) == 19:
                glyphs[bitmap] = chr(character)
        if len(glyphs) < 90:
            raise ValueError("unexpected UDK2018 font")
        width, height, pixels = rgb_pixels(path)
        candidates = [(80, 25), (80, 50), (100, 31), (width // 8, height // 19)]
        best = ""
        for columns, rows in candidates:
            if columns * 8 > width or rows * 19 > height:
                continue
            left, top = (width - columns * 8) // 2, (height - rows * 19) // 2
            lines = []
            for row in range(rows):
                line = ""
                for column in range(columns):
                    samples = [
                        pixels[top + row * 19 + y][left + column * 8 : left + column * 8 + 8]
                        for y in range(19)
                    ]
                    background = Counter(p for scanline in samples for p in scanline).most_common(
                        1
                    )[0][0]
                    bitmap = bytes(
                        sum((p != background) << (7 - i) for i, p in enumerate(scanline))
                        for scanline in samples
                    )
                    line += glyphs.get(bitmap, "?")
                lines.append(line.rstrip())
            text = "\n".join(lines)
            if text.count("?") < best.count("?") or not best:
                best = text
            normalized = " ".join(text.split())
            if all(" ".join(word.split()) in normalized for word in words):
                return True, text
        return False, best
    except (OSError, ValueError, IndexError, struct.error, zlib.error) as error:
        return False, str(error)
