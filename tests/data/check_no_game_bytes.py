#!/usr/bin/env python3
"""No JSON file in data/ or tools/lift/ carries bytes of the game: a field whose name contains "bytes" may only hold
a size (an integer, e.g. "element_bytes": 4), never the bytes themselves. Sites and entries give a length instead;
the bytes are read from the player's own files."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def offenders(value, path):
    if isinstance(value, dict):
        for key, item in value.items():
            where = f"{path}.{key}"
            if "bytes" in key.lower() and (isinstance(item, bool) or not isinstance(item, int)):
                yield where

            yield from offenders(item, where)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            yield from offenders(item, f"{path}[{index}]")


def main():
    files = sorted(ROOT.glob("data/**/*.json")) + sorted(ROOT.glob("tools/lift/**/*.json"))
    found = []
    for file in files:
        for where in offenders(json.loads(file.read_text()), "$"):
            found.append(f"{file.relative_to(ROOT)}: {where}")

    for line in found:
        print(f"game bytes field: {line}", file=sys.stderr)

    print(f"checked {len(files)} JSON files, {len(found)} fields with bytes")
    return 1 if found or not files else 0


if __name__ == "__main__":
    sys.exit(main())
