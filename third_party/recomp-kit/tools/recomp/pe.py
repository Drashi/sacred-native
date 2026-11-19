"""A small reader of 32-bit PE images: what the translator needs and nothing more.

It replaces the pefile package (a dependency that would travel to every player's machine) and keeps its results for
the three files this port translates: the memory image (headers, then each section's raw data at its virtual
address, as pefile's get_memory_mapped_image builds it), HIGHLOW base relocations, the import address table names
and the section table. tests/lift/test_pe.py compares it with pefile where pefile is installed.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass


class PEError(ValueError):
    pass


@dataclass(frozen=True)
class Section:
    name: str
    virtual_address: int
    virtual_size: int
    raw_size: int
    raw_pointer: int
    characteristics: int


# Base relocation types (PE/COFF specification 6.6.2).
REL_ABSOLUTE = 0
REL_HIGHLOW = 3


class PE:
    def __init__(self, data: bytes):
        self.data = bytearray(data)
        if data[:2] != b"MZ":
            raise PEError("no MZ header")
        nt = self._u32(0x3C)
        if data[nt:nt + 4] != b"PE\0\0":
            raise PEError("no PE signature")
        machine, sections, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, nt + 4)
        if machine != 0x14C:
            raise PEError(f"not an i386 image (machine {machine:#x})")
        optional = nt + 24
        if self._u16(optional) != 0x10B:
            raise PEError("not a PE32 optional header")
        self.image_base = self._u32(optional + 28)
        self.section_alignment = self._u32(optional + 32)
        self.file_alignment = self._u32(optional + 36)
        self.size_of_image = self._u32(optional + 56)
        self.size_of_headers = self._u32(optional + 60)
        count = self._u32(optional + 92)
        self.directories = [struct.unpack_from("<II", data, optional + 96 + 8 * i) for i in range(min(count, 16))]
        table = optional + optional_size
        self.sections: list[Section] = []
        for i in range(sections):
            raw = struct.unpack_from("<8sIIIIIIHHI", data, table + 40 * i)
            self.sections.append(Section(raw[0].rstrip(b"\0").decode("ascii", "replace"), raw[2], raw[1], raw[3],
                                         raw[4], raw[9]))
        # The headers run to the end of the section table, or to the first section's raw data if that lies further.
        table_end = table + 40 * sections
        lowest = min((s.raw_pointer & ~0x1FF for s in self.sections if s.raw_pointer), default=0)
        self.header_end = lowest if lowest and lowest >= table_end else table_end

    def _u16(self, offset: int) -> int:
        return struct.unpack_from("<H", self.data, offset)[0]

    def _u32(self, offset: int) -> int:
        return struct.unpack_from("<I", self.data, offset)[0]

    def directory(self, index: int) -> tuple[int, int]:
        """(rva, size) of a data directory; (0, 0) when the image has none."""
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def _aligned_va(self, va: int) -> int:
        # pefile rounds a section's address down to the section alignment (the file alignment below a page).
        alignment = self.section_alignment if self.section_alignment >= 0x1000 else self.file_alignment
        return va - va % alignment if alignment else va

    def _offset(self, rva: int) -> int:
        """File offset of an RVA inside a section's raw data or the headers."""
        if rva < self.header_end:
            return rva
        for s in self.sections:
            va = self._aligned_va(s.virtual_address)
            if va <= rva < va + max(s.virtual_size, s.raw_size):
                if rva - va >= s.raw_size:
                    raise PEError(f"rva {rva:#x} has no file data")
                return (s.raw_pointer & ~0x1FF) + rva - va
        raise PEError(f"rva {rva:#x} is in no section")

    def memory_image(self) -> bytes:
        """The headers, then each section's raw data at its virtual address (zero-filled between); it ends at the
        last section's raw end, which may be short of SizeOfImage."""
        image = bytearray(self.data[:self.header_end])
        for s in self.sections:
            if s.virtual_size == 0 and s.raw_size == 0:
                continue
            start = s.raw_pointer & ~0x1FF
            if s.raw_size > len(self.data) or start + s.raw_size > len(self.data):
                continue
            va = self._aligned_va(s.virtual_address)
            if va < len(image):
                del image[va:]
            else:
                image += bytes(va - len(image))
            image += self.data[start:start + s.raw_size]
        return bytes(image)

    def relocations(self) -> list[tuple[int, int]]:
        """(rva, type) of every base relocation entry."""
        rva, size = self.directory(5)
        if not size:
            return []
        out = []
        at = self._offset(rva)
        end = at + size
        while at + 8 <= end:
            page, block = struct.unpack_from("<II", self.data, at)
            if block < 8:
                break
            for i in range((block - 8) // 2):
                entry = self._u16(at + 8 + 2 * i)
                out.append((page + (entry & 0xFFF), entry >> 12))
            at += block
        return out

    def relocate(self, new_base: int) -> None:
        """Apply the HIGHLOW relocations for a new image base to the section data (the headers keep the old base, as
        pefile leaves them)."""
        delta = new_base - self.image_base
        for rva, kind in self.relocations():
            if kind == REL_ABSOLUTE:
                continue
            if kind != REL_HIGHLOW:
                raise PEError(f"relocation type {kind} at rva {rva:#x} is not supported")
            at = self._offset(rva)
            struct.pack_into("<I", self.data, at, (self._u32(at) + delta) & 0xFFFFFFFF)
        self.image_base = new_base

    def _string(self, rva: int) -> bytes:
        at = self._offset(rva)
        end = self.data.index(0, at)
        return bytes(self.data[at:end])

    def imports(self) -> dict[int, str]:
        """{address of an import address table slot: imported name}, for the imports by name (pefile also names
        some imports by ordinal from its own tables; the translator only looks for imports by name)."""
        rva, size = self.directory(1)
        names: dict[int, str] = {}
        if not size:
            return names
        at = self._offset(rva)
        while True:
            lookup, _, _, name, first = struct.unpack_from("<IIIII", self.data, at)
            if not (lookup or name or first):
                break
            thunk = lookup or first
            for i in range(0x10000):
                value = self._u32(self._offset(thunk + 4 * i))
                if value == 0:
                    break
                if not value & 0x80000000:
                    names[self.image_base + first + 4 * i] = self._string(value + 2).decode("ascii", "replace")
            at += 20
        return names
