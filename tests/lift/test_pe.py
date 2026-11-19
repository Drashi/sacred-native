"""The translator's PE reader (third_party/recomp-kit/tools/recomp/pe.py) against pefile, on the game's three files
(SACRED_ASSETS) where pefile is installed, and on a made-up image everywhere."""
import os
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "third_party/recomp-kit/tools/recomp"))
import pe  # noqa: E402

FILES = ("Sacred.exe", "granny.dll", "tincat2.dll")
ASSETS = Path(os.environ["SACRED_ASSETS"]) if os.environ.get("SACRED_ASSETS") else None


def made_up_image() -> bytes:
    """Headers, one code section at 0x1000 holding a pointer at 0x1004, a relocation section, an import of
    KERNEL32!ExitProcess whose slot is at 0x1010."""
    image = bytearray(0x800)
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", image, 0x84, 0x14C, 2, 0, 0, 0, 0xE0, 0x102)
    opt = 0x98
    struct.pack_into("<H", image, opt, 0x10B)
    struct.pack_into("<I", image, opt + 28, 0x400000)            # ImageBase
    struct.pack_into("<II", image, opt + 32, 0x1000, 0x200)      # alignments
    struct.pack_into("<II", image, opt + 56, 0x3000, 0x200)      # SizeOfImage, SizeOfHeaders
    struct.pack_into("<I", image, opt + 92, 16)
    struct.pack_into("<II", image, opt + 96 + 8 * 1, 0x1040, 40)  # imports
    struct.pack_into("<II", image, opt + 96 + 8 * 5, 0x2000, 12)  # relocations
    table = opt + 0xE0
    struct.pack_into("<8sIIIIIIHHI", image, table, b".text", 0x100, 0x1000, 0x200, 0x200, 0, 0, 0, 0, 0x60000020)
    struct.pack_into("<8sIIIIIIHHI", image, table + 40, b".reloc", 12, 0x2000, 0x200, 0x400, 0, 0, 0, 0, 0x42000040)
    text = 0x200
    struct.pack_into("<I", image, text + 4, 0x401010)             # a pointer the loader rebases
    struct.pack_into("<II", image, text + 0x10, 0x1080, 0)        # IAT: hint/name at 0x1080
    struct.pack_into("<II", image, text + 0x20, 0x1080, 0)        # lookup table
    struct.pack_into("<IIIII", image, text + 0x40, 0x1020, 0, 0, 0x10A0, 0x1010)
    image[text + 0x82:text + 0x8E] = b"ExitProcess\0"
    image[text + 0xA0:text + 0xAD] = b"KERNEL32.dll\0"
    struct.pack_into("<IIHH", image, 0x400, 0x1000, 12, 0x3004, 0)
    return bytes(image)


def test_made_up_image():
    image = pe.PE(made_up_image())
    assert image.image_base == 0x400000 and image.size_of_image == 0x3000
    assert [s.name for s in image.sections] == [".text", ".reloc"]
    assert image.imports() == {0x401010: "ExitProcess"}
    assert image.relocations() == [(0x1004, 3), (0x1000, 0)]
    mapped = image.memory_image()
    assert len(mapped) == 0x2200 and struct.unpack_from("<I", mapped, 0x1004)[0] == 0x401010
    image.relocate(0x500000)
    assert struct.unpack_from("<I", image.memory_image(), 0x1004)[0] == 0x501010


@pytest.mark.skipif(ASSETS is None, reason="SACRED_ASSETS is not set")
@pytest.mark.parametrize("filename", FILES)
def test_same_as_pefile(filename):
    pefile = pytest.importorskip("pefile")
    raw = (ASSETS / filename).read_bytes()
    ours, theirs = pe.PE(raw), pefile.PE(data=raw)
    assert ours.image_base == theirs.OPTIONAL_HEADER.ImageBase
    assert ours.size_of_image == theirs.OPTIONAL_HEADER.SizeOfImage
    assert ours.directory(5) == (theirs.OPTIONAL_HEADER.DATA_DIRECTORY[5].VirtualAddress,
                                 theirs.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size)
    assert [(s.name, s.virtual_address, s.virtual_size, s.raw_size, s.characteristics) for s in ours.sections] == [
        (s.Name.rstrip(b"\0").decode(), s.VirtualAddress, s.Misc_VirtualSize, s.SizeOfRawData, s.Characteristics)
        for s in theirs.sections]
    assert ours.memory_image() == theirs.get_memory_mapped_image()
    theirs.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    assert ours.imports() == {imp.address: imp.name.decode("ascii", "replace")
                              for entry in getattr(theirs, "DIRECTORY_ENTRY_IMPORT", [])
                              for imp in entry.imports if imp.name and not imp.import_by_ordinal}
    for base in (0x10000000, 0x11000000):
        ours, theirs = pe.PE(raw), pefile.PE(data=raw)
        ours.relocate(base)
        theirs.relocate_image(base)
        assert ours.memory_image() == theirs.get_memory_mapped_image()
