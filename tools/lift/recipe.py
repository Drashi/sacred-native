#!/usr/bin/env python3
"""The translation recipe: what generate.py needs from the Ghidra export, without any byte of the game.

generate.py reads three things from an export: each module's SHA-256, the order and length of every exported
instruction, and the basic-block leaders (where a region may be cut). The instruction bytes themselves come from
the player's own files (generate.py checks them against the export and decodes them with capstone). A recipe keeps
only those facts, so the C can be generated from the player's Sacred.exe, granny.dll and tincat2.dll without Ghidra.

File: gzip JSON, schema "sacred-native-recipe" version 1:
  {"modules": {"sacred": {"file": "Sacred.exe", "sha256": ..., "instructions": N,
                          "runs": [[rva, "lengths"], ...], "leaders": [delta, ...]}, ...}}
  runs     the listing in export order, cut where the next instruction does not start where the previous ended:
           rva of the run's first instruction and one hex digit per instruction length (x86: 1..15 bytes)
  leaders  block-leader RVAs, sorted, as differences to the previous one (the first to 0)

  recipe.py make [--export DIR] [--out FILE]   recipe from an export (default analysis/export -> data/recipe.json.gz)
  recipe.py check RECIPE --assets DIR [--export DIR]
        the player's files match the recipe's hashes; with --export, the recipe lists exactly the export's listing
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Final

ROOT: Final = Path(__file__).resolve().parents[2]
SCHEMA: Final = "sacred-native-recipe"
VERSION: Final = 1
# generate.py's module names, the files they translate and the export directory each comes from.
MODULES: Final = (("sacred", "Sacred.exe", "sacred-startup"), ("granny", "granny.dll", "granny"),
                  ("tincat", "tincat2.dll", "tincat2"))


@dataclass(frozen=True, slots=True)
class Listing:
    """One module's translation facts: generate.py's whole view of an export."""
    sha256: str
    leaders: frozenset[int]                 # RVAs
    instructions: tuple[tuple[int, int], ...]  # (rva, length) in export order


def from_export(directory: Path) -> Listing:
    manifest = json.loads((directory / "manifest.json").read_text())
    leaders: set[int] = set()
    with gzip.open(directory / "blocks.jsonl.gz", "rt") as blocks:
        for line in blocks:
            leaders.update(int(entry["rva"], 16) for entry in json.loads(line)["entries"] if "rva" in entry)

    instructions = []
    with gzip.open(directory / "instructions.jsonl.gz", "rt") as stream:
        for line in stream:
            item = json.loads(line)
            instructions.append((int(item["address"]["rva"], 16), len(item["bytes"]) // 2))

    return Listing(manifest["sha256"], frozenset(leaders), tuple(instructions))


def encode(listing: Listing, filename: str) -> dict:
    runs: list[list] = []
    end = None
    for rva, length in listing.instructions:
        if not 1 <= length <= 15:
            raise SystemExit(f"{filename}: instruction at rva {rva:#x} is {length} bytes long")

        if rva != end:
            runs.append([rva, []])

        runs[-1][1].append(f"{length:x}")
        end = rva + length

    previous = 0
    deltas = []
    for rva in sorted(listing.leaders):
        deltas.append(rva - previous)
        previous = rva

    return {"file": filename, "sha256": listing.sha256, "instructions": len(listing.instructions),
            "runs": [[rva, "".join(lengths)] for rva, lengths in runs], "leaders": deltas}


def decode(entry: dict) -> Listing:
    instructions = []
    for rva, lengths in entry["runs"]:
        for digit in lengths:
            length = int(digit, 16)
            instructions.append((rva, length))
            rva += length

    if len(instructions) != entry["instructions"]:
        raise SystemExit(f"recipe: {entry['file']} lists {len(instructions)} instructions, "
                         f"its header says {entry['instructions']}")

    leaders, rva = set(), 0
    for delta in entry["leaders"]:
        rva += delta
        leaders.add(rva)

    return Listing(entry["sha256"], frozenset(leaders), tuple(instructions))


def load(path: Path) -> dict[str, tuple[str, Listing]]:
    """Module name -> (file name, listing)."""
    with gzip.open(path, "rt") as stream:
        data = json.load(stream)

    if data.get("schema") != SCHEMA or data.get("version") != VERSION:
        raise SystemExit(f"{path}: not a {SCHEMA} version {VERSION} file")

    return {name: (entry["file"], decode(entry)) for name, entry in data["modules"].items()}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make(export: Path, out: Path) -> None:
    modules = {name: encode(from_export(export / directory), filename) for name, filename, directory in MODULES}
    data = {"schema": SCHEMA, "version": VERSION, "modules": modules}
    out.parent.mkdir(parents=True, exist_ok=True)
    # mtime=0: the same export gives the same file.
    with open(out, "wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=0) as stream:
        stream.write(json.dumps(data, separators=(",", ":")).encode())

    sizes = ", ".join(f"{name} {entry['instructions']} instructions in {len(entry['runs'])} runs"
                      for name, entry in modules.items())
    print(f"{out}: {out.stat().st_size} bytes; {sizes}")


def check(recipe: Path, assets: Path, export: Path | None) -> int:
    failed = 0
    for name, (filename, listing) in load(recipe).items():
        source = assets / filename
        if not source.is_file():
            print(f"{filename}: missing in {assets}")
            failed += 1
            continue

        if digest(source) != listing.sha256:
            print(f"{filename}: SHA-256 {digest(source)} is not the recipe's {listing.sha256} (another version?)")
            failed += 1
            continue

        if export is not None:
            directory = dict((n, d) for n, _, d in MODULES)[name]
            if from_export(export / directory) != listing:
                print(f"{filename}: the recipe differs from {export / directory}")
                failed += 1
                continue

        print(f"{filename}: ok ({len(listing.instructions)} instructions, {len(listing.leaders)} leaders)")

    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("make")
    p.add_argument("--export", type=Path, default=ROOT / "analysis/export")
    p.add_argument("--out", type=Path, default=ROOT / "data/recipe.json.gz")
    p = commands.add_parser("check")
    p.add_argument("recipe", type=Path)
    p.add_argument("--assets", type=Path, required=True)
    p.add_argument("--export", type=Path)
    args = parser.parse_args()
    if args.command == "make":
        make(args.export, args.out)
        return 0

    return check(args.recipe, args.assets, args.export)


if __name__ == "__main__":
    sys.exit(main())
