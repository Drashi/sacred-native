"""Generate bounded IA-32 fixtures and reconcile every exported computed site."""
from pathlib import Path
import gzip
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import capstone

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("SACRED_GEN_DIR", ROOT / "build/tests/control"))
EXPORT = Path(os.environ.get("SACRED_EXPORT_DIR", ROOT / "analysis/export"))
sys.path.insert(0, str(ROOT / "third_party/recomp-kit/tools/recomp/tests"))
from test_translate_insns import Case, NoImage, Opts, T, translate_case
from test_emission import mismatch_case


def command(args: list[str]) -> bytes:
    return subprocess.run(args, cwd=ROOT, check=True, capture_output=True, timeout=180).stdout


def translated(name: str, address: int, raw: bytes, entries: tuple[int, ...] = ()) -> str:
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    image = T.Image.__new__(T.Image)
    insns = [image.to_insn(i) for i in decoder.disasm(raw, address)]
    assert sum(i.size for i in decoder.disasm(raw, address)) == len(raw), name
    case = Case(name, address, [(i.addr-address, i.raw.split("  ", 1)[1]) for i in insns],
                raw.hex(), entries=entries)
    return translate_case(case)


def main() -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    T.RESUMABLE_STACKS = T.DRIVER_CHECKPOINTS = True
    # Never inherit another game's address-based CRT substitutions.
    T.INTRINSIC_BODY = {}
    command(["x86_64-linux-gnu-as", "--32", "tests/control/fixtures.S", "-o", str(BUILD / "fixtures.o")])
    command(["x86_64-linux-gnu-ld", "-m", "elf_i386", "--section-start=.guest=0x03000000",
             "-e", "cdecl_sum", str(BUILD / "fixtures.o"), "-o", str(BUILD / "fixtures.elf")])
    command(["x86_64-linux-gnu-objcopy", "-O", "binary", "--only-section=.guest",
             str(BUILD / "fixtures.elf"), str(BUILD / "fixtures.bin")])
    raw = (BUILD / "fixtures.bin").read_bytes()
    symbols: list[tuple[int, int, str]] = []
    for row in command(["x86_64-linux-gnu-nm", "-S", str(BUILD / "fixtures.elf")]).decode().splitlines():
        words = row.split()
        if len(words) == 4 and words[2] == "T":
            symbols.append((int(words[0], 16), int(words[1], 16), words[3]))

    parts = ['#include "x86.h"', '#include "seh.h"', '#include "intrinsics.h"']
    header = ["#pragma once", "#include <stdint.h>"]
    table: list[str] = []
    for address, size, name in sorted(symbols):
        offset = address - 0x03000000
        # Every CALL continuation is a block entry, including nonlocal longjmp resumes.
        decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        entries = tuple(i.address + i.size for i in decoder.disasm(raw[offset:offset+size], address)
                        if i.mnemonic == "call" and i.address+i.size < address+size)
        if name in ("setjmp_intrinsic", "longjmp_intrinsic"):
            helper = "recomp_guest_setjmp" if name == "setjmp_intrinsic" else "recomp_guest_longjmp"
            parts.append(f"void fn_{address:08x}(X86 *c) {{ {helper}(c); }}")
        else:
            parts.append(translated(name, address, raw[offset:offset+size], entries))

        header.append(f"constexpr uint32_t fixture_{name} = 0x{address:08x};")
        kinds = 1 if name == "cdecl_sum" else 3
        table.append(f'{{0x{address:08x}u, fn_{address:08x}, "{name}", {kinds}}},')
        for entry in entries:
            table.append(f'{{0x{entry:08x}u, fn_{entry:08x}, "{name} continuation", 2}},')

    original = mismatch_case()
    case = Case(original.name, 0x04001000,
                [(off, text.replace("0x601080", "0x4001080")) for off, text in original.lines],
                original.code.replace(struct.pack("<I", 0x601080), struct.pack("<I", 0x4001080)).hex())
    parts.append(translate_case(case))
    table.append('{0x04001000u, fn_04001000, "rebased synthetic mismatch reproducer", 3},')
    header.append("constexpr uint32_t fixture_mismatch = 0x04001000;")
    # The reproducer's handler address is a separate, admitted guest entry.
    parts.append('void fn_04001080(X86 *c) { recomp_jump(c, 0x%08xu); }' %
                 next(a for a, _, n in symbols if n == "continuation_handler"))
    table.append('{0x04001080u, fn_04001080, "mismatch handler", 3},')

    manifest = json.loads((EXPORT / "manifest.json").read_text())
    digest = hashlib.sha256()
    calls = jumps = boundaries = 0
    quarantined: list[dict[str, str | int]] = []
    throw_raw = bytearray()
    site_forms: set[tuple[str, str]] = set()
    with gzip.open(EXPORT / "instructions.jsonl.gz", "rt") as rows, \
         (BUILD / "boundaries.bin").open("wb") as boundary_file, \
         (BUILD / "sites.jsonl").open("w") as sites:
        for row in rows:
            digest.update(row.encode())
            item = json.loads(row)
            address = 0x400000 + int(item["address"]["rva"], 16)
            boundary_file.write(struct.pack("<I", address))
            boundaries += 1
            if 0x849ea5 <= address <= 0x849ede:
                throw_raw.extend(bytes.fromhex(item["bytes"]))

            flow = item["flow_type"]
            if "COMPUTED" not in flow or not ("CALL" in flow or "JUMP" in flow):
                continue

            is_call = "CALL" in flow
            calls += is_call
            jumps += not is_call
            target = "recomp_call" if is_call else "recomp_jump"
            if item["mnemonic"] in ("JMPF", "CALLF"):
                # These are byte-verified *candidates*, not qualified Win32 code.
                # Never reinterpret a far pointer as a near call or drop the site.
                quarantined.append({"source_pc": address, "bytes": item["bytes"],
                                    "reason": "unqualified far control-transfer candidate"})
                sites.write(json.dumps({"source_pc": address, "bytes": item["bytes"],
                                        "route": "strict_quarantine", "destinations": item["flows"],
                                        "admission": "not callable; dispatcher must trap"}) + "\n")
                continue

            # Use the actual address/operand/bytes, not merely a mnemonic census.
            try:
                text = translated("site", address, bytes.fromhex(item["bytes"]) +
                                  (b"\xc3" if is_call else b""))
            except T.TranslateError as error:
                raise RuntimeError(f"computed site {address:#x} {item['mnemonic']} "
                                   f"{item['bytes']} {item['operands'][0]['text']}") from error

            assert target + "(c," in text, (hex(address), text)
            site_forms.add((flow, item["operands"][0]["text"]))
            sites.write(json.dumps({"source_pc": address, "bytes": item["bytes"],
                                    "route": target, "destinations": item["flows"],
                                    "admission": "candidate; compiled target required"}) + "\n")

    assert (calls, jumps, boundaries) == (12191, 1010, 1265230)
    assert manifest["sha256"] == "db9b86a44c8c1cc8c5ed9e3f427121ab8b4ed62760861e58995f10af9d971e74"
    parts.append(translated("original_CxxThrowException", 0x849ea5, bytes(throw_raw)))
    table.append('{0x849ea5u, fn_00849ea5, "original Sacred CxxThrowException", 3},')
    parts += ["struct FixtureEntry { uint32_t address; void (*function)(X86*); const char *name; uint8_t kinds; };",
              "const struct FixtureEntry control_fixtures[] = {", *table, "};",
              "const unsigned control_fixture_count = sizeof control_fixtures / sizeof control_fixtures[0];"]
    text = "\n".join(parts)
    assert re.search(r"\bsetjmp\(", text) is None and "jmp_buf" not in text
    (BUILD / "translated.c").write_text(text)
    (BUILD / "fixtures.h").write_text("\n".join(header) + "\n")
    receipt = {"module_sha256": manifest["sha256"], "computed_calls": calls,
               "computed_jumps": jumps, "instruction_boundaries": boundaries,
               "site_operand_forms": len(site_forms), "ir_stream_sha256": digest.hexdigest(),
               "translated_functions": len(symbols) + 3,
               "original_throw_va": "0x00849ea5", "original_throw_bytes": throw_raw.hex(),
               "native_checkpoint_count_in_generated_code": 0}
    receipt["quarantined_far_sites"] = quarantined
    (BUILD / "dispatch-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt), flush=True)


if __name__ == "__main__":
    main()
