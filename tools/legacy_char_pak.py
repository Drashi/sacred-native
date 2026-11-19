#!/usr/bin/env python3
"""Install a repainted character-select backdrop for LEGACY_UI into a profile.

The original Sacred backdrop (GUI_CHAR_01..12, 4x3 tiles of 256x256 in PAK/texture.pak) has no places for the two
Underworld classes. This writes <profile>/PAK/texture03.pak: the installation's texture03.pak (which the game loads
after texture.pak) plus twelve GUI_CHAR_xx entries cut from a 1024x768 PNG. The profile overlay then serves it in
place of the installation's file; nothing in the installation changes. GUI_CHAR_* is only drawn with LEGACY_UI on,
so the Gold screens are unaffected. Delete the written file to go back to the original backdrop.

Usage: legacy_char_pak.py INSTALLATION BACKDROP.png PROFILE
"""
from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

TILE = 256
COLUMNS, ROWS = 4, 3
HEADER = 0x50
INDEX = 0x100
FORMAT_BGRA = 6


def read_png(path: Path) -> tuple[int, int, bytes]:
    """8-bit RGB or RGBA, non-interlaced; returns width, height and RGBA rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")

    at, idat, header = 8, b"", None
    while at < len(data):
        length, kind = struct.unpack_from(">I4s", data, at)
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body

        at += 12 + length

    if not header:
        raise SystemExit(f"{path}: no IHDR")

    width, height, depth, color, _, _, interlace = header
    if depth != 8 or color not in (2, 6) or interlace:
        raise SystemExit(f"{path}: needs 8-bit RGB or RGBA without interlacing")

    channels = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous, at = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
            elif kind:
                raise SystemExit(f"{path}: bad filter {kind}")

        rows.append(bytes(line))
        previous = line

    if channels == 3:
        rows = [b"".join(r[i:i + 3] + b"\xff" for i in range(0, len(r), 3)) for r in rows]

    return width, height, b"".join(rows)


def read_pak(path: Path) -> list[tuple[str, bytes]]:
    data = path.read_bytes()
    if data[:4] != b"TEX\x03":
        raise SystemExit(f"{path}: not a texture pak")

    entries = []
    for i in range(struct.unpack_from("<I", data, 4)[0]):
        _, offset, size = struct.unpack_from("<III", data, INDEX + 12 * i)
        if size:
            entries.append((data[offset:offset + 32].split(b"\0")[0].decode("latin-1"),
                            data[offset:offset + HEADER + size]))

    return entries


def tile_entry(name: str, rgba: bytes, width: int, column: int, row: int) -> bytes:
    pixels = bytearray()
    for y in range(TILE):
        start = ((row * TILE + y) * width + column * TILE) * 4
        line = bytearray(rgba[start:start + TILE * 4])
        line[0::4], line[2::4] = line[2::4], line[0::4]  # RGBA -> BGRA
        pixels += line

    header = name.encode().ljust(32, b"\0") + struct.pack("<HHI", TILE, TILE, FORMAT_BGRA | len(pixels) << 8)
    return header.ljust(HEADER, b"\0") + bytes(pixels)


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)

    installation, backdrop, profile = map(Path, sys.argv[1:])
    width, height, rgba = read_png(backdrop)
    if (width, height) != (COLUMNS * TILE, ROWS * TILE):
        raise SystemExit(f"{backdrop}: {width}x{height}, needs {COLUMNS * TILE}x{ROWS * TILE}")

    source = next((p for p in (installation / "PAK").iterdir() if p.name.lower() == "texture03.pak"), None) \
        if (installation / "PAK").is_dir() else None
    names = {f"GUI_CHAR_{i + 1:02d}.TGA" for i in range(COLUMNS * ROWS)}
    entries = [e for e in (read_pak(source) if source else []) if e[0].upper() not in names]
    for i in range(COLUMNS * ROWS):
        entries.append((f"GUI_CHAR_{i + 1:02d}.TGA",
                        tile_entry(f"GUI_CHAR_{i + 1:02d}.TGA", rgba, width, i % COLUMNS, i // COLUMNS)))

    count = len(entries) + 1  # entry 0 stays empty, as in the original paks
    offset = INDEX + 12 * count
    index, blob = bytearray(12), bytearray()
    for _, entry in entries:
        index += struct.pack("<III", 4, offset + len(blob), len(entry) - HEADER)
        blob += entry

    head = bytearray(INDEX)
    head[:12] = b"TEX\x03" + struct.pack("<II", count, len(blob))
    folder = next((p for p in profile.iterdir() if p.name.lower() == "pak" and p.is_dir()), profile / "PAK")
    folder.mkdir(parents=True, exist_ok=True)
    target = folder / (source.name if source else "texture03.pak")
    target.write_bytes(bytes(head) + bytes(index) + bytes(blob))
    print(f"{target}: {len(entries)} textures")


if __name__ == "__main__":
    main()
