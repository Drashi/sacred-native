#!/usr/bin/env python3
"""Builds the game module (runtime/modules/game_module.h) on the player's machine: the launcher runs this once, and
again after an update. Nothing of the game is shipped: the C is generated from the recipe and the player's own
Sacred.exe, granny.dll and tincat2.dll, then compiled into one shared library the runtime loads.

    build_game.py --assets GAME --recipe recipe.json.gz --out DIR [--cc CC]... [--jobs N] [--qa 0|1]

--cc names a C compiler command ("cc", "zig cc"); given several times, the first one that compiles and links a
probe with the module's flags is used, so the system compiler can go first and the bundled Zig after it. The
default is $CC, else cc.

Progress goes to stdout, one line each, for the launcher to show:
    COMPILER <command>          the compiler chosen
    STEP <name> <text>          verify | generate | compile | link
    PROGRESS <done> <total>
    DONE <path of the module>
    ERROR <text>
The module is DIR/libsacred_game.so (.dylib on macOS); DIR/module.stamp holds the hash of every input, and an up to
date module is not rebuilt.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import platform
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/lift"))
import recipe as RECIPE  # noqa: E402

# The compile contract of the CMake game module (CMakeLists.txt, SACRED_GAME_MODULE): keep the two in step.
DEFINES = ["-DRECOMP_MMU_GUEST=1", "-DRECOMP_NO_STORE_HOOKS=1", "-DSACRED_DRIVER_CHECKPOINTS=1"]
FLAGS = ["-std=gnu11", "-O1", "-g0", "-fexceptions", "-fno-fast-math", "-ffp-contract=off", "-fPIC",
         "-fvisibility=hidden", "-w"]
INCLUDES = [".", "third_party/recomp-kit/runtime", "third_party/bochs", "third_party/bochs/softfloat3e/include"]
# Inputs besides the recipe and the player's files that change the module.
INPUT_FILES = ["tools/lift/generate.py", "tools/lift/recipe.py", "tools/lift/resolution.py",
               "tools/lift/resolution_sites.json", "tools/lift/x87_fast.py", "tools/lift/promote.py",
               "runtime/sacred/native.c", "runtime/granny/native.c", "runtime/modules/game_module.h",
               "runtime/modules/resolution_table.h", "third_party/recomp-kit/runtime/x86.h",
               "third_party/recomp-kit/runtime/seh.h", "third_party/recomp-kit/runtime/intrinsics.h"]


def say(*words: object) -> None:
    print(*words, flush=True)


def module_name() -> str:
    return "libsacred_game.dylib" if platform.system() == "Darwin" else "libsacred_game.so"


def supports(cc: list[str], flag: str, work: Path) -> bool:
    probe = work / "probe.c"
    probe.write_text("int x;\n")
    result = subprocess.run([*cc, flag, "-c", str(probe), "-o", str(work / "probe.o")],
                            capture_output=True)
    return result.returncode == 0


def developer_tools() -> bool:
    """macOS: whether Xcode or its command line tools are installed."""
    try:
        return subprocess.run(["xcode-select", "-p"], capture_output=True, timeout=30).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def usable(cc: list[str], work: Path) -> bool:
    """Whether a compiler builds a shared library with the module's flags (and exists at all)."""
    if platform.system() == "Darwin" and "/" not in cc[0] and not developer_tools():
        return False  # /usr/bin/cc and friends are stubs that open the installer of the command line tools

    probe = work / "probe.c"
    probe.write_text("#include <stdint.h>\n#include <string.h>\n#include <math.h>\n"
                     "__attribute__((visibility(\"default\"))) uint64_t probe(double x) "
                     "{ char b[8]; memcpy(b, &x, 8); return (uint64_t)b[0] + (uint64_t)floor(x); }\n")
    try:
        compiled = subprocess.run([*cc, *FLAGS, *DEFINES, "-c", str(probe), "-o", str(work / "probe.o")],
                                  capture_output=True, timeout=300)
        if compiled.returncode:
            return False

        link = [*cc, "-shared", "-o", str(work / "probe.so"), str(work / "probe.o")]
        if platform.system() == "Darwin":
            link[len(cc):len(cc) + 1] = ["-dynamiclib", "-undefined", "dynamic_lookup"]

        return subprocess.run(link, capture_output=True, timeout=300).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False
    finally:
        for name in ("probe.c", "probe.o", "probe.so"):
            (work / name).unlink(missing_ok=True)


def stamp_of(args: argparse.Namespace, cc: list[str]) -> str:
    digest = hashlib.sha256()
    for name in INPUT_FILES:
        digest.update(name.encode() + b"\0" + (ROOT / name).read_bytes())

    for path in sorted((ROOT / "third_party/recomp-kit/tools/recomp").glob("*.py")):
        digest.update(path.read_bytes())

    for path in sorted((ROOT / "runtime/cpu").glob("*.h")):
        digest.update(path.read_bytes())

    digest.update(args.recipe.read_bytes())
    # The compiler by what it is, not where it lies or how its path is quoted.
    version = subprocess.run([*cc, "--version"], capture_output=True, text=True).stdout
    digest.update(version.encode() + " ".join(FLAGS + DEFINES).encode() + f"qa={args.qa}".encode())
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cc", action="append", default=[])
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    parser.add_argument("--qa", type=int, default=0)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    module = out / module_name()
    try:
        candidates = args.cc or [os.environ.get("CC") or "cc"]
        cc = next((shlex.split(c) for c in candidates if usable(shlex.split(c), out)), None)
        if cc is None:
            raise RuntimeError("no working C compiler (tried " + ", ".join(candidates) + ")")

        say("COMPILER", shlex.join(cc))
        say("STEP verify Checking the game files")
        for name, (filename, listing) in RECIPE.load(args.recipe).items():
            source = args.assets / filename
            if not source.is_file():
                raise RuntimeError(f"{filename} is missing in {args.assets}")

            if RECIPE.digest(source) != listing.sha256:
                raise RuntimeError(f"{filename} is not the supported version (Sacred Gold 2.28, English)")

        stamp = stamp_of(args, cc)
        stamp_file = out / "module.stamp"
        if not args.force and module.is_file() and stamp_file.is_file() and stamp_file.read_text().strip() == stamp:
            say("DONE", module)
            return 0

        say("STEP generate Translating the game code")
        generated = out / "generated"
        env = dict(os.environ, SACRED_ASSETS=str(args.assets), SACRED_RECIPE=str(args.recipe.resolve()),
                   SACRED_GENERATED_DIR=str(generated))
        result = subprocess.run([sys.executable, str(ROOT / "tools/lift/generate.py")], env=env,
                                capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError("translation failed:\n" + (result.stderr or result.stdout)[-2000:])

        sources = [Path(p) for p in (generated / "sources.cmake").read_text().split('"')[1::2]]
        sources.append(ROOT / "runtime/sacred/native.c")

        flags = list(FLAGS)
        if platform.machine() in ("aarch64", "arm64") and supports(cc, "-mno-outline", out):
            flags.append("-mno-outline")  # see CMakeLists.txt: the outliner costs 40% of game-thread time

        if args.qa:
            flags.append("-DSACRED_QA=1")

        includes = [f"-I{ROOT / directory}" for directory in INCLUDES]
        objects = out / "objects"
        objects.mkdir(exist_ok=True)
        say("STEP compile Compiling")
        say("PROGRESS 0", len(sources))

        def compile_one(source: Path) -> Path:
            target = objects / (source.parent.name + "_" + source.stem + ".o")
            command = [*cc, *flags, *DEFINES, *includes, "-c", str(source), "-o", str(target)]
            done = subprocess.run(command, capture_output=True, text=True)
            if done.returncode:
                raise RuntimeError(f"compiling {source.name} failed:\n{done.stderr[-2000:]}")

            return target

        built = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for count, target in enumerate(pool.map(compile_one, sources), 1):
                built.append(target)
                say("PROGRESS", count, len(sources))

        say("STEP link Linking")
        listing = out / "objects.txt"
        listing.write_text("\n".join(shlex.quote(str(p)) for p in built) + "\n")
        link = [*cc, "-shared", "-o", str(module) + ".tmp", f"@{listing}"]
        if platform.system() == "Darwin":
            link[len(cc):len(cc) + 1] = ["-dynamiclib", "-undefined", "dynamic_lookup"]

        done = subprocess.run(link, capture_output=True, text=True)
        if done.returncode:
            raise RuntimeError("linking failed:\n" + done.stderr[-2000:])

        os.replace(str(module) + ".tmp", module)
        stamp_file.write_text(stamp + "\n")
        say("DONE", module)
        return 0
    except Exception as error:  # the launcher shows the text
        for line in str(error).splitlines() or [repr(error)]:
            say("ERROR", line)

        return 1


if __name__ == "__main__":
    sys.exit(main())
