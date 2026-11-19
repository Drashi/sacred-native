#!/usr/bin/env python3
"""Digest of the C that tools/lift/generate.py writes, to check that a change of the translator's machinery (its PE
reader, its instruction decoder, the Python it runs on) leaves the generated code byte for byte as it was.

    generated_digest.py compute DIR            # print the digests of a generated tree
    generated_digest.py check DIR [--digest F] # compare with data/generated-digest.json; exit 1 on a difference
    generated_digest.py write DIR [--digest F] # record a tree as the reference (after an intended change)
    generated_digest.py run [--out DIR]        # generate (SACRED_ASSETS, SACRED_RECIPE or the export) and check

The digest holds hashes only: nothing of the game. Every file counts except the receipts (receipt.json names the
hashes of the translator's own sources, which change with any edit); sources.cmake counts with the output directory
replaced, so a tree generated elsewhere compares equal.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIGEST = ROOT / "data/generated-digest.json"
# Receipt fields that do not depend on the translator's sources: they must stay as they are too.
RECEIPT_FIELDS = ("module", "sha256", "assigned_base", "original_base", "instructions", "regions", "shards",
                  "quarantined")


def compute(tree: Path) -> dict[str, str]:
    """{group: sha256} per module directory, for the top-level files, and a total."""
    tree = tree.resolve()
    groups: dict[str, hashlib._Hash] = {}
    for path in sorted(p for p in tree.rglob("*") if p.is_file()):
        relative = path.relative_to(tree)
        group = relative.parts[0] if len(relative.parts) > 1 else "top"
        data = path.read_bytes()
        if path.name == "receipt.json":
            receipt = json.loads(data)
            data = json.dumps({k: receipt.get(k) for k in RECEIPT_FIELDS}, sort_keys=True).encode()
        elif path.name == "sources.cmake":
            data = data.replace(str(tree).encode(), b"@OUT@")

        digest = groups.setdefault(group, hashlib.sha256())
        digest.update(str(relative).encode() + b"\0" + len(data).to_bytes(8, "little") + data)

    result = {group: digest.hexdigest() for group, digest in sorted(groups.items())}
    total = hashlib.sha256("".join(f"{k}={v}\n" for k, v in result.items()).encode())
    return {**result, "total": total.hexdigest()}


def check(tree: Path, reference: Path) -> int:
    expected = json.loads(reference.read_text())
    got = compute(tree)
    if got == expected:
        print(f"generated C identical to {reference.name}: {got['total']}")
        return 0

    for group in sorted(set(expected) | set(got)):
        if expected.get(group) != got.get(group):
            print(f"differs: {group}: expected {expected.get(group)}, got {got.get(group)}", file=sys.stderr)

    return 1


def run(out: Path, reference: Path) -> int:
    env = {k: v for k, v in os.environ.items()
           if k not in ("SACRED_X87_RUNS", "SACRED_DIRECT_CALLS", "SACRED_EXPORT_DIR")}
    if not env.get("SACRED_ASSETS"):
        print("generated_digest: SACRED_ASSETS is not set", file=sys.stderr)
        return 2

    env["SACRED_GENERATED_DIR"] = str(out)
    done = subprocess.run([sys.executable, str(ROOT / "tools/lift/generate.py")], env=env,
                          stdout=subprocess.DEVNULL)
    if done.returncode:
        return done.returncode

    return check(out, reference)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("compute", "check", "write", "run"))
    parser.add_argument("tree", nargs="?", type=Path)
    parser.add_argument("--digest", type=Path, default=DIGEST)
    parser.add_argument("--out", type=Path, default=ROOT / "build/generated-digest")
    args = parser.parse_args()
    if args.command == "run":
        return run(args.out.resolve(), args.digest)

    if args.tree is None:
        parser.error("a generated tree is needed")

    if args.command == "compute":
        print(json.dumps(compute(args.tree), indent=2))
        return 0

    if args.command == "write":
        args.digest.write_text(json.dumps(compute(args.tree), indent=2) + "\n")
        print(f"wrote {args.digest}")
        return 0

    return check(args.tree, args.digest)


if __name__ == "__main__":
    sys.exit(main())
