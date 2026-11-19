"""The translator's x86 decoder (third_party/recomp-kit/tools/recomp/x86dec.py) against capstone: on every
instruction of the game's three files where SACRED_ASSETS, a recipe (SACRED_RECIPE, default data/recipe.json.gz)
and capstone are at hand, and on a few made-up instructions everywhere."""
import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "third_party/recomp-kit/tools/recomp"))
sys.path.insert(0, str(ROOT / "tools/lift"))
import pe  # noqa: E402
import x86dec  # noqa: E402

ASSETS = Path(os.environ["SACRED_ASSETS"]) if os.environ.get("SACRED_ASSETS") else None
RECIPE = Path(os.environ.get("SACRED_RECIPE") or ROOT / "data/recipe.json.gz")
BASES = {"sacred": 0x400000, "granny": 0x10000000, "tincat": 0x11000000}


def shape(insn) -> tuple:
    """What Translator.to_insn and generate.py read of an instruction; registers by name."""
    name = insn.reg_name
    operands = []
    for op in insn.operands:
        if op.type == x86dec.OP_REG:
            operands.append(("reg", name(op.reg)))
        elif op.type == x86dec.OP_IMM:
            operands.append(("imm", op.imm))
        else:
            m = op.mem
            operands.append(("mem", op.size, name(m.segment) if m.segment else None, name(m.base) if m.base else None,
                             name(m.index) if m.index else None, m.scale if m.index else 1, m.disp))

    return insn.address, insn.size, insn.mnemonic, tuple(operands)


def ours(code: bytes, pc: int) -> list:
    return [shape(i) for i in x86dec.Decoder().disasm(code, pc)]


@pytest.mark.parametrize("code, expected", [
    ("8b442404", ("mov", (("reg", "eax"), ("mem", 4, None, "esp", None, 1, 4)))),
    ("83e0f0", ("and", (("reg", "eax"), ("imm", 0xFFFFFFF0)))),
    ("83c0f0", ("add", (("reg", "eax"), ("imm", -0x10)))),
    ("6aff", ("push", (("imm", -1),))),
    ("f3a5", ("rep movsd", (("mem", 4, "es", "edi", None, 1, 0), ("mem", 4, None, "esi", None, 1, 0)))),
    ("d9c9", ("fxch", (("reg", "st(0)"), ("reg", "st(1)")))),
    ("dce1", ("fsubr", (("reg", "st(1)"), ("reg", "st(0)")))),
    ("0fc2c104", ("cmpneqps", (("reg", "xmm0"), ("reg", "xmm1")))),
    ("0f0fd1b4", ("pfmul", (("reg", "mm2"), ("reg", "mm1")))),
    ("64a100000000", ("mov", (("reg", "eax"), ("mem", 4, "fs", None, None, 1, 0)))),
    ("e8fbffffff", ("call", (("imm", 0x1000),))),
])
def test_made_up(code, expected):
    (got,) = ours(bytes.fromhex(code), 0x1000)
    assert got[1] == len(code) // 2 and got[2:] == expected


def test_wait_prefix_is_its_own_instruction():
    assert [i[2] for i in ours(bytes.fromhex("9bdfe0"), 0)] == ["wait", "fnstsw"]


def test_undecodable_stops():
    assert ours(bytes.fromhex("0f0b"), 0) == []


@pytest.mark.skipif(ASSETS is None or not RECIPE.is_file(), reason="needs SACRED_ASSETS and a recipe")
def test_same_as_capstone_on_the_game():
    capstone = pytest.importorskip("capstone")
    import recipe
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    checked = 0
    differences = []
    for name, (filename, listing) in recipe.load(RECIPE).items():
        image = pe.PE((ASSETS / filename).read_bytes())
        image.relocate(BASES[name])
        data = image.memory_image()
        for rva, length in listing.instructions:
            code, pc = data[rva:rva + length], BASES[name] + rva
            theirs = [shape(i) for i in md.disasm(code, pc)]
            if ours(code, pc) != theirs:
                differences.append((filename, hex(pc), code.hex(), theirs, ours(code, pc)))

            checked += 1

    assert checked > 1_000_000
    assert not differences, f"{len(differences)} differences, first: {differences[:5]}"
