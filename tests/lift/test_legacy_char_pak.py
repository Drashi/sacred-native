"""tools/legacy_char_pak.py: a 1024x768 PNG cut into GUI_CHAR_01..12 entries of a profile texture03.pak."""
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import legacy_char_pak as L  # noqa: E402


def png(path, width, height):
    """Row 0 holds (x % 256, 7, 9); every other row repeats it through the Up filter."""
    first = b"\0" + bytes(v for x in range(width) for v in (x % 256, 7, 9))
    rows = first + (b"\x02" + bytes(3 * width)) * (height - 1)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def test_tiles_replace_the_backdrop_and_keep_the_installation_entries(tmp_path):
    installation, profile = tmp_path / "game", tmp_path / "profile"
    (installation / "PAK").mkdir(parents=True)
    (profile / "Pak").mkdir(parents=True)
    head = bytearray(0x100)
    head[:12] = b"TEX\x03" + struct.pack("<II", 2, 0)
    entry = b"CAB.TGA".ljust(32, b"\0") + struct.pack("<HHI", 1, 1, 4 | 3 << 8)
    entry = entry.ljust(0x50, b"\0") + b"abc"
    (installation / "PAK" / "texture03.pak").write_bytes(
        bytes(head) + bytes(12) + struct.pack("<III", 4, 0x100 + 24, 3) + entry)
    png(tmp_path / "b.png", 1024, 768)
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/legacy_char_pak.py"), str(installation),
                    str(tmp_path / "b.png"), str(profile)], check=True, capture_output=True)
    written = profile / "Pak" / "texture03.pak"
    entries = dict(L.read_pak(written))
    assert list(entries) == ["CAB.TGA"] + [f"GUI_CHAR_{i:02d}.TGA" for i in range(1, 13)]
    assert entries["CAB.TGA"] == entry
    tile = entries["GUI_CHAR_02.TGA"]
    assert struct.unpack_from("<HHI", tile, 32) == (256, 256, 6 | 0x40000 << 8)
    # Tile 2 starts at x = 256 of the first row: BGRA of (256 % 256, 7, 9).
    assert tile[0x50:0x54] == bytes((9, 7, 0, 255))
    assert tile[0x54:0x58] == bytes((9, 7, 1, 255))
