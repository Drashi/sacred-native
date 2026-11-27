"""Stream the complete exported listing through the admitted recomp-kit emitter."""
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
import hashlib
import json
import os
import re
import sys
from typing import Final


ROOT: Final = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "third_party/recomp-kit/tools/recomp"))
import translate as T
import pe as PE
import x86dec
import x87_fast
import resolution as R
import promote
import recipe as RECIPE

# F_: the per-instruction code behind an x87 run, T_: behind the tick of a block that a run enters from a lowered
# call, V_: behind the verify run of such a call (x87_fast).
_LABEL: Final = re.compile(r"([LFTV]_[0-9a-f]{8}): ;")
_GOTO: Final = re.compile(r"\bgoto ([LFTV]_[0-9a-f]{8});")


@dataclass(frozen=True, slots=True)
class Module:
    name: str
    filename: str
    base: int
    export: Path


EXPORT: Final = Path(os.environ.get("SACRED_EXPORT_DIR", ROOT / "analysis/export"))
ASSETS: Final = Path(os.environ["SACRED_ASSETS"]) if os.environ.get("SACRED_ASSETS") else None
# SACRED_RECIPE (tools/lift/recipe.py): generate from a recipe and the player's files instead of the Ghidra export.
RECIPE_FILE: Final = Path(os.environ["SACRED_RECIPE"]) if os.environ.get("SACRED_RECIPE") else None

MODULES: Final = (
    Module("sacred", "Sacred.exe", 0x400000, EXPORT / "sacred-startup"),
    Module("granny", "granny.dll", 0x10000000, EXPORT / "granny"),
    Module("tincat", "tincat2.dll", 0x11000000, EXPORT / "tincat2"),
)
BUILD: Final = Path(os.environ.get("SACRED_GENERATED_DIR", ROOT / "build/generated"))
RESOLUTION_SITES: Final = ROOT / "tools/lift/resolution_sites.json"
# Native stand-ins (runtime/sacred/native.c) that a direct CALL rel32 of these Sacred.exe functions runs instead of
# their region code. Every other way in still runs the generated code.
NATIVE_CALLS: Final = {0x00640410: "sacred_native_00640410",
                        # the ground mesh (runtime/ddraw/ground_mesh.cpp)
                        0x0062ae90: "sacred_native_0062ae90", 0x0062d3c0: "sacred_native_0062d3c0",
                        0x00629180: "sacred_native_00629180", 0x006292c0: "sacred_native_006292c0"}
# A native check in front of an instruction that, when it returns nonzero, continues the guest code at another
# address of the same function: (target, C condition). The condition may read guest registers and memory but
# takes no `c` (promote.py keeps the registers in locals across it).
NATIVE_JUMPS: Final = {
    # renderTileRow: the tile's ground quad build (from `fld [1.0]; fdiv [ebp+0xb60]` to the test after it) is
    # skipped while the ground mesh draws the ground, except for water and lava tiles (type byte [esp+0x64]).
    0x0062b103: (0x0062b7ff, "sacred_ground_skip_build(rd8(c->r[4] + 0x64u))"),
}
# The C runtime's _ftol of Sacred.exe, which the x87 runs take in as an operation (x87_fast.Compiler.call): entry ->
# (length and FNV-1a of its code, offset of its FISTP m64, offset of its RET, instructions a call executes). The
# lowering is tied to those bytes: with any others the call stays a call.
X87_FTOL: Final = {0x0084a1f4: (39, 0x09a81b5a, 0x19, 0x26, 15)}
STAND_INS: Final = frozenset(int(a, 16) for a in re.findall(
    r"^STAND_IN\(\d+, ([0-9a-f]{8})\)", (ROOT / "runtime/granny/native.c").read_text(), re.M))
# SACRED_DIRECT_CALLS=0 generates the flat form (every CALL and RET through the driver) for comparison.
DIRECT_CALLS: Final = os.environ.get("SACRED_DIRECT_CALLS", "1") != "0"


def _parse_operand_with_cell_displacement(parse):
    """A resolution "disp" site marks its memory operand with R.DISP_MARK and a cell address: the operand keeps its
    registers and reads the displacement from the cell (the address expression replaces the constant)."""

    def wrapped(text):
        if R.HD_READ_MARK in text:
            operand, cells = text.split(R.HD_READ_MARK)
            flag, cell = (int(x, 16) for x in cells.split(","))
            op = parse(operand)
            if op.kind != "mem":
                raise T.TranslateError(f"mode-only read on a non-memory operand {text!r}")

            op.addr_c = f"(rd32({flag:#x}u) ? {cell:#x}u : {T.addr_expr(op)})"
            return op

        if R.DISP_MARK not in text:
            return parse(text)

        operand, cell = text.split(R.DISP_MARK)
        op = parse(operand)
        if op.kind != "mem":
            raise T.TranslateError(f"cell displacement on a non-memory operand {text!r}")

        op.disp = 0
        op.addr_c = f"({T.addr_expr(op)} + rd32({int(cell, 16):#x}u))"
        return op

    return wrapped


T.parse_operand = _parse_operand_with_cell_displacement(T.parse_operand)


def unchanged_write(path: Path, text: str) -> None:
    """Preserve timestamps so an unchanged generation is an incremental no-op."""
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


class RegionTranslator(T.Translator):
    """Bounded C regions yield every outward transfer to the existing C driver.

    No host call stack or inferred function boundary owns guest control flow.
    All non-control instructions still use the pinned instruction templates.
    """

    def goto_target(self, fn, target, ins):
        if target in fn.index:
            return [f"goto L_{target:08x};"]

        return [self.leave(target)]

    def leave(self, target: int) -> str:
        """Leaving the region for `target`: a tail call of the region that holds it when that is code of the same
        module (no driver round trip, no host stack growth), otherwise back to the driver."""
        region = self.direct_region.get(target)
        if region is None:
            return f"c->eip = 0x{target:08x}u; return;"

        self.called.add(region)
        return f"c->eip = 0x{target:08x}u; if (RECOMP_DIRECT_OK(c)) RECOMP_DIRECT_JUMP(c, region_{region:08x}); return;"

    #: Direct calls (set per module by generate()): the region function of every listed instruction address, and
    #: the region functions the shard being written calls.
    direct_region: dict[int, int] = {}
    called: set[int] = set()
    natives: set[str] = set()
    #: Routines a run may take in as an operation (set per module by generate()): entry -> x87_fast.Ftol.
    x87_calls: dict[int, x87_fast.Ftol] = {}

    def _emit(self, fn, index, ins, mnemonic, next_pc, live):
        if mnemonic == "CALL" and (target := self.branch_target(ins)) in self.direct_region:
            # A CALL rel32 to code of the same module runs the callee's region as a C call instead of returning to
            # the driver twice (dispatch of the callee, dispatch of the return address). It continues here only when
            # the callee came back to this return address with no non-local transfer pending; anything else (and a
            # disabled or too deep direct path) is the driver's, exactly as without it (seh.h RECOMP_DIRECT_*).
            region = self.direct_region[target]
            callee = NATIVE_CALLS.get(target, f"region_{region:08x}")
            if target in NATIVE_CALLS:
                self.natives.add(callee)
            else:
                self.called.add(region)

            back = f"goto L_{next_pc:08x};" if next_pc in fn.index else "return;"
            # The verify mode of a lowered routine compares its run with what the call left (x87_fast.h).
            verify = ("if (RECOMP_UNLIKELY(x87f_ftol_mode == 2u)) x87f_ftol_verify_end(c); "
                      if target in self.x87_calls else "")
            return [f"c->r[4] -= 4; wr32(c->r[4], 0x{next_pc:08x}u);",
                    f"c->eip = 0x{target:08x}u;",
                    f"if (RECOMP_DIRECT_OK(c)) {{ RECOMP_DIRECT_CALL(c, {callee}); {verify}"
                    f"if (RECOMP_DIRECT_BACK(c, 0x{next_pc:08x}u)) {back} }}",
                    "return;"]

        if mnemonic == "CALL":
            operand = T.parse_operand(ins.ops[0])
            lines = [f"uint32_t target_ = {T.read_op(operand, 32)};",
                     f"c->r[4] -= 4; wr32(c->r[4], 0x{next_pc:08x}u);",
                     "c->eip = target_;"]
            if self.direct_region:
                # An indirect call (virtual functions, imports): the callee's compiled code through this thread's
                # dispatch cache, the same lookup the driver would make; a host function stays the driver's.
                back = f"goto L_{next_pc:08x};" if next_pc in fn.index else "return;"
                lines.append("if (RECOMP_DIRECT_OK(c)) { void (*f_)(X86 *) = recomp_direct_target(c, target_); "
                             f"if (f_) {{ RECOMP_DIRECT_CALL(c, f_); "
                             f"if (RECOMP_DIRECT_BACK(c, 0x{next_pc:08x}u)) {back} }} }}")

            return lines + ["return;"]

        if mnemonic == "JMP" and self.branch_target(ins) is None:
            if self.direct_region:
                return [f"c->eip = {T.read_op(T.parse_operand(ins.ops[0]), 32)};",
                        "if (RECOMP_DIRECT_OK(c)) { void (*f_)(X86 *) = recomp_direct_target(c, c->eip); "
                        "if (f_) RECOMP_DIRECT_JUMP(c, f_); }",
                        "return;"]

            return [f"c->eip = {T.read_op(T.parse_operand(ins.ops[0]), 32)}; return;"]

        return super()._emit(fn, index, ins, mnemonic, next_pc, live)


# -- arithmetic-flag liveness inside one region --------------------------------
# The emitter writes every defined flag eagerly. Inside a region the next readers of a flag are visible, so a flag
# store that every path overwrites before any read is dropped. Exactness rule: at every instruction that reads a flag
# (Jcc, SETcc, CMOVcc, ADC/SBB, PUSHFD, any unlisted instruction) the value equals the eager build's. Hence:
#   * only instructions whose emitted code itself writes a flag unconditionally kill it (KILLS, written inline by
#     flags_arith/flags_logic; logic ops never write AF, so they do not kill it);
#   * an instruction not known to be flag-neutral reads every flag;
#   * every exit from the region (return to the dispatcher, call, indirect or out-of-region branch) reads every flag.
# Only a fault taken between a dropped store and its overwrite can observe the difference (the stale flag in the
# guest exception CONTEXT); Sacred's handlers never inspect EFlags.
_ALL = T.ALL_FLAGS
_NONE: Final = frozenset()
_KILLS: Final = {
    **dict.fromkeys(("ADD", "SUB", "CMP", "NEG", "ADC", "SBB", "XADD", "CMPXCHG"), _ALL),
    **dict.fromkeys(("INC", "DEC"), _ALL - {"cf"}),
    **dict.fromkeys(("AND", "OR", "XOR", "TEST"), _ALL - {"af"}),
}
# Instructions whose emitted code consults `live` only to skip dead flag stores (see Translator._emit).
_LIVE_EMIT: Final = frozenset(_KILLS) | {"SHL", "SHR", "SAR", "ROL", "ROR", "RCL", "RCR", "BT"}
_NEUTRAL: Final = frozenset((
    "MOV", "MOVZX", "MOVSX", "LEA", "PUSH", "POP", "XCHG", "NOP", "NOT", "CDQ", "CWD", "CBW", "CWDE", "BSWAP",
    "LEAVE", "MOVSB", "MOVSW", "MOVSD", "STOSB", "STOSW", "STOSD", "LODSB", "LODSW", "LODSD",
    "SHL", "SHR", "SAR", "ROL", "ROR", "SHLD", "SHRD", "BTS", "BTR", "BTC"))
_EXITS: Final = frozenset(("CALL", "RET", "RETN", "JMP", "INT", "INT3", "INTO", "HLT", "UD2", "IRET", "IRETD"))


def flag_effect(ins: T.Insn) -> tuple[frozenset, frozenset]:
    """(kills, reads) of the arithmetic flags, by the rule above."""
    m = ins.mnem
    if m in T.JCC:
        return _NONE, frozenset(T.JCC[m][1])

    if m in T.SETCC:
        return _NONE, frozenset(T.SETCC[m][1])

    if m in T.CMOVCC:
        return _NONE, frozenset(T.CMOVCC[m][1])

    if ins.rep:
        return _NONE, _ALL

    if m in _KILLS:
        return _KILLS[m], frozenset(("cf",)) if m in ("ADC", "SBB") else _NONE

    if m in ("RCL", "RCR"):
        return _NONE, frozenset(("cf",))

    if m == "IMUL" and len(ins.ops) >= 2 or m == "BT":
        return _NONE, _NONE

    if m in _NEUTRAL or (m.startswith("F") and not m.startswith(("FCMOV", "FCOMI", "FUCOMI"))):
        return _NONE, _NONE

    return _NONE, _ALL


def region_liveness(instructions: list, fallthrough: list[int]) -> list[frozenset]:
    """Flags live after each instruction of one region (backward fixed point over the region's own CFG)."""
    index = {ins.addr: i for i, ins in enumerate(instructions)}
    n = len(instructions)
    effects = [flag_effect(ins) for ins in instructions]
    successors: list[list[int] | None] = []
    for i, ins in enumerate(instructions):
        falls = fallthrough[i] if i + 1 < n and fallthrough[i] == instructions[i + 1].addr else None
        target = T.Translator.branch_target(ins)
        if ins.mnem in _EXITS and not (ins.mnem == "JMP" and target in index):
            successors.append(None)                       # leaves the region: everything is live
        elif ins.mnem == "JMP":
            successors.append([index[target]])
        elif ins.mnem in T.JCC:
            successors.append(None if falls is None or target not in index else [i + 1, index[target]])
        else:
            successors.append(None if falls is None else [i + 1])

    live_in = [_NONE] * n
    live_out = [_NONE] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = _ALL if successors[i] is None else frozenset().union(*(live_in[s] for s in successors[i]))
            kills, reads = effects[i]
            new_in = (out - kills) | reads
            if out != live_out[i] or new_in != live_in[i]:
                live_out[i], live_in[i] = out, new_in
                changed = True

    return live_out


def tick_points(instructions: list, fallthrough: list[int], leaders: set[int]) -> dict[int, int]:
    """Instruction index -> instructions it accounts for in the scheduling budget.

    A tick opens every block (exported leader, branch target, first instruction after a transfer) and covers the
    straight run up to the next tick. Transfers (CALL/JMP/RET) get their own tick so the host sees the exact source
    address of a call, as it did with one tick per instruction."""
    n = len(instructions)
    targets = {T.Translator.branch_target(ins) for ins in instructions}
    starts = []
    for i, ins in enumerate(instructions):
        prev = instructions[i - 1] if i else None
        if (i == 0 or ins.addr in leaders or ins.addr in targets or ins.mnem in _EXITS or prev.mnem in _EXITS
                or prev.mnem in T.JCC or fallthrough[i - 1] != ins.addr):
            starts.append(i)

    return {s: (starts[k + 1] if k + 1 < len(starts) else n) - s for k, s in enumerate(starts)}


# -- instruction-pointer stores ----------------------------------------------------------------------------------
# c->eip is read only by region exits (which set it), by helpers that raise guest exceptions or walk SEH (x87_wait,
# DIV/IDIV, recomp_* and SEH registration) and, as a fatal diagnostic, by a failed guest memory access. Every block
# start stores it, so a yield or a block entry sees an exact address; inside a block an instruction whose emitted
# code calls nothing outside these pure helpers skips the store (a fatal memory fault then reports its block start).
_PURE_HELPER: Final = re.compile(
    r"(rd|wr)(8|16|32|64|f32|f64)|parity8|x87_(get|set|push|load|store|binary|integer|compare|unary|emms)|fdrop|fxch|"
    r"fstsw|bswap32|memcpy|(mmx|simd|xmm)_\w+|(shl|shr|sar|rol|ror|rcl|rcr)(8|16|32)(_f)?|(shld|shrd)32_f|"
    r"imul2_(16|32)(_f)?|(imul|mul)(8|16|32)|(rep_|repe_|repne_)?(movs|stos|lods|scas|cmps)[bwd]")
_KEYWORDS: Final = frozenset(("if", "for", "while", "switch", "sizeof", "return"))
_CALL: Final = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def needs_eip(emitted: list[str]) -> bool:
    """Whether the emitted code of one instruction may read c->eip."""
    for line in emitted:
        code = line.split("/*", 1)[0]
        for name in _CALL.findall(code):
            if name not in _KEYWORDS and not _PURE_HELPER.fullmatch(name):
                return True

    return False


def normalized(image: T.Image, decoded: list[x86dec.Insn]) -> T.Insn:
    """Keep Ghidra's wait-prefixed x87 instruction as one qualified operation."""
    if len(decoded) == 1:
        return image.to_insn(decoded[0])

    if len(decoded) == 2 and decoded[0].mnemonic == "wait" and decoded[1].mnemonic.startswith("f"):
        ins = image.to_insn(decoded[1])
        mnemonic = {"FNSTCW": "FSTCW", "FNSTSW": "FSTSW", "FNSTENV": "FSTENV",
                    "FNSAVE": "FSAVE", "FNINIT": "FINIT", "FNCLEX": "FCLEX"}.get(ins.mnem, ins.mnem)
        pc = decoded[0].address
        return T.Insn(pc, mnemonic, ins.rep, ins.ops,
                      f"{pc:08x}  {mnemonic} " + ",".join(ins.ops))

    raise T.TranslateError("exported instruction decodes into multiple unqualified operations")


X87_RUNS: Final = os.environ.get("SACRED_X87_RUNS", "1") != "0"


# Guest memory accesses of a region go through the guest base and page table it loads once (x86.h
# RECOMP_LOCAL_MEMORY): read from the globals, both are loaded again before every access, since a guest store may
# alias them.
LOCAL_ACCESSORS: Final = "".join(
    f"#define {name}(a) {name}_in(gm_, pt_, (a))\n"
    for name in ("rd8", "rd16", "rd32", "rd64", "rdf32", "rdf64")) + "".join(
    f"#define {name}(a, v) {name}_in(gm_, pt_, (a), (v))\n"
    for name in ("wr8", "wr16", "wr32", "wr64", "wrf32", "wrf64"))


def emit_instruction(tr, fn, i: int, ins, live_out, quarantined) -> list[str]:
    try:
        emitted = tr.emit(fn, i, live_out[i] if ins.mnem in _LIVE_EMIT and not ins.rep else T.ALL_FLAGS)
    except T.TranslateError as error:
        quarantined.append({"pc": ins.addr, "listing": ins.raw, "reason": str(error)})
        emitted = [f"recomp_unmodelled(c, 0x{ins.addr:08x}u); return;"]

    if any("recomp_unmodelled" in line for line in emitted):
        if not quarantined or quarantined[-1]["pc"] != ins.addr:
            quarantined.append({"pc": ins.addr, "listing": ins.raw,
                                "reason": "unqualified emitter form"})

    if any(helper in line for line in emitted for helper in
           ("recomp_rdtsc(", "recomp_in(", "recomp_out(", "recomp_cli(", "recomp_sti(")):
        quarantined.append({"pc": ins.addr, "listing": ins.raw,
                            "reason": "strict unqualified machine operation"})

    return emitted


def region_body(tr, fn, instructions: list, leaders: set[int], quarantined: list, hook_lines=None,
                x87_runs: bool = X87_RUNS) -> list[str]:
    """The labelled statements of one region: per-instruction code, with native-double x87 runs in front of it.
    hook_lines(ins, emitted) returns the resolution hook statements placed (before, after) an instruction."""
    live_out = region_liveness(instructions, fn.fallthrough)
    ticks = tick_points(instructions, fn.fallthrough, leaders)
    n = len(instructions)
    emitted = [emit_instruction(tr, fn, i, ins, live_out, quarantined) for i, ins in enumerate(instructions)]
    hooks = {i: hook for i, ins in enumerate(instructions)
             if hook_lines and (hook := hook_lines(ins, emitted[i])) != ([], [])}

    def falls(i: int) -> bool:
        return i + 1 < n and fn.fallthrough[i] == instructions[i + 1].addr

    def exit_lines(i: int) -> list[str]:
        return [f"goto L_{instructions[i + 1].addr:08x};"] if falls(i) else [tr.leave(fn.fallthrough[i])]

    def usable(i: int) -> bool:
        # A hooked instruction keeps its own code: a native x87 run would skip the hook.
        return i not in hooks and not any("recomp_unmodelled" in line for line in emitted[i])

    def inert_at(k: int) -> bool:
        return x87_fast.inert(instructions[k], emitted[k], needs_eip)

    # Direct calls of a routine the runs take in (x87_fast.Call). A run goes on behind such a call when the next
    # instruction follows in this region: that instruction starts a block, whose tick the run counts.
    calls: dict[int, x87_fast.Call] = {}
    for i, ins in enumerate(instructions):
        routine = tr.x87_calls.get(tr.branch_target(ins)) if x87_runs and ins.mnem == "CALL" else None
        if routine is not None and usable(i):
            calls[i] = x87_fast.Call(routine, fn.fallthrough[i], ticks[i + 1] if falls(i) else None)

    runs: dict[int, list[str]] = {}
    i = 0
    while x87_runs and i < n:
        if (instructions[i].mnem in x87_fast.SUPPORTED or i in calls) and usable(i):
            limit = i + 1
            while (limit < n and falls(limit - 1) and usable(limit) and
                   (limit not in ticks or limit in calls or limit - 1 in calls)):
                limit += 1

            run = x87_fast.compile_run(tr.image, instructions, emitted, i, limit, inert_at, exit_lines, calls)
            if run is not None:
                runs[i] = run[1]
                i = run[0] + 1
                continue

        i += 1

    # The verify mode's run of each lowered call alone, in front of the call's own code.
    verify = {i: x87_fast.compile_run(tr.image, instructions, emitted, i, i + 1, inert_at,
                                      lambda k: ["x87f_ftol_verify_inline(c);"], calls, verify=True)[1]
              for i in sorted(calls)}

    # Where the region can be entered (its switch): block starts, which are also every address a jump, a call's
    # return or the scheduler resumes at, and the instructions that may raise a guest exception (a handler that
    # continues execution resumes at the faulting instruction). Nothing enters a block in the middle otherwise, and
    # without those labels in the switch the compiler keeps guest registers in host registers across a block.
    fn.entries = sorted(set(ticks) | {i for i in range(n) if needs_eip(emitted[i])})
    body: list[str] = []
    for i, ins in enumerate(instructions):
        body += [f"L_{ins.addr:08x}: ;"]
        if i in ticks:
            # A transfer's block keeps the exact EIP and source the host sees (a host call's source, the call
            # trace); any other block only counts its instructions. Its EIP is stored where its first instruction
            # may read it, like any instruction inside a block.
            transfer = ins.mnem in _EXITS
            if transfer or i in hooks or needs_eip(emitted[i]):
                body += [f"c->eip = 0x{ins.addr:08x}u;"]

            body += [(f"recomp_source(c, 0x{ins.addr:08x}u);" if ticks[i] == 1 else
                      f"recomp_block(c, 0x{ins.addr:08x}u, {ticks[i]}u);") if transfer else
                     f"recomp_tick(c, {ticks[i]}u);"]
            if i - 1 in calls and i not in calls:
                body += [f"T_{ins.addr:08x}: ;"]  # where a run that came in over the call hands back

        before, after = hooks.get(i, ([], []))
        body += runs.get(i, [])
        body += verify.get(i, [])
        if i not in ticks and needs_eip(emitted[i]):
            body += [f"c->eip = 0x{ins.addr:08x}u;"]

        body += before
        body += emitted[i]
        body += after
        if not falls(i):
            body += [tr.leave(fn.fallthrough[i])]

    return body


def generate(module: Module) -> list[str]:
    """Validate source bytes, relocate actual HIGHLOW sites, and pack C shards."""
    out = BUILD / module.name
    out.mkdir(parents=True, exist_ok=True)
    if RECIPE_FILE is not None:
        filename, listing = RECIPE.load(RECIPE_FILE)[module.name]
        assert filename == module.filename, f"recipe: {module.name} is {filename}"
        ir_sha256 = blocks_sha256 = RECIPE.digest(RECIPE_FILE)
    else:
        listing = RECIPE.from_export(module.export)
        ir_sha256 = RECIPE.digest(module.export / "instructions.jsonl.gz")
        blocks_sha256 = RECIPE.digest(module.export / "blocks.jsonl.gz")

    if ASSETS is None:
        raise SystemExit("SACRED_ASSETS must point at the original Sacred Gold installation")

    source = ASSETS / module.filename
    raw = source.read_bytes()
    if hashlib.sha256(raw).hexdigest() != listing.sha256:
        raise SystemExit(f"{source} does not match the exported module hash")

    image = T.Image(str(source))
    pe = PE.PE(raw)
    original_base = pe.image_base
    original = pe.memory_image()
    pe.relocate(module.base)
    relocated = pe.memory_image()
    image.base = module.base
    image.end = module.base + image.size
    image.data = relocated + bytes(max(0, image.size - len(relocated)))
    decoder = x86dec.Decoder()
    tr = RegionTranslator(image, set(), SimpleNamespace(eager_flags=True))
    tr.allow_unmodelled = None
    leaders = {module.base + rva for rva in listing.leaders}
    # Direct calls between the regions of the main executable: the regions are cut exactly as below (at a block
    # leader once a region has 128 instructions), so every listed address maps to the function that will hold it.
    tr.direct_region = {}
    tr.called = set()
    tr.x87_calls = {}
    if module.filename in ("Sacred.exe", "granny.dll") and DIRECT_CALLS:
        start, length = None, 0
        for rva, _ in listing.instructions:
            pc = module.base + rva
            if start is None or (length >= 128 and pc in leaders):
                start, length = pc, 0

            tr.direct_region[pc] = start
            length += 1

        for address in STAND_INS:  # replaced in the dispatch table by runtime/granny/native.c
            tr.direct_region.pop(address, None)

        if module.filename == "Sacred.exe":
            for entry, (length, fnv, fistp, ret, count) in X87_FTOL.items():
                code = relocated[entry - module.base:entry - module.base + length]
                digest = 0x811c9dc5
                for byte in code:
                    digest = ((digest ^ byte) * 0x01000193) & 0xffffffff

                if digest == fnv and entry in tr.direct_region and entry not in NATIVE_CALLS:
                    opcode = ((code[fistp] & 7) << 8) | code[fistp + 1]
                    tr.x87_calls[entry] = x87_fast.Ftol(entry + fistp, opcode, entry + ret, count)

    # Resolution-dependent sites (an opt-in screen size): only the main executable, pinned to its hash.
    resolution = None
    if module.filename == "Sacred.exe":
        try:
            resolution = R.load(RESOLUTION_SITES, original, original_base)
            R.check_image(resolution, raw, original, original_base)
        except R.TableError as error:
            raise SystemExit(f"{RESOLUTION_SITES}: {error}") from None

    resolution_used: set[int] = set()
    hooks_used: set[int] = set()

    def hook_lines(ins, emitted: list[str]) -> tuple[list[str], list[str]]:
        option = resolution.options.get(ins.addr) if resolution else None
        if option:
            if any(word in line for line in emitted for word in ("return;", "goto ")):
                raise SystemExit(f"{RESOLUTION_SITES}: option {ins.addr:#x} is on a control transfer")

            hooks_used.add(ins.addr)
            return [], R.option_c(option)

        jump = NATIVE_JUMPS.get(ins.addr) if module.filename == "Sacred.exe" else None
        if jump:
            hooks_used.add(ins.addr)
            target, condition = jump
            if target not in leaders:
                raise SystemExit(f"native jump {ins.addr:#x} -> {target:#x}: the target is no block start")

            return [f"if ({condition}) {{ {tr.leave(target)} }}"], []

        hook = resolution.hooks.get(ins.addr) if resolution else None
        if not hook:
            return [], []

        hooks_used.add(ins.addr)
        if hook.before:
            return R.hook_c(resolution, hook), []

        if any(word in line for line in emitted for word in ("return;", "goto ")):
            raise SystemExit(f"{RESOLUTION_SITES}: hook {ins.addr:#x} is on a control transfer")

        return [], R.hook_c(resolution, hook)

    quarantined: list[dict[str, str | int]] = []
    paths: list[str] = []
    shards: list[str] = []
    table: list[str] = []
    instructions: list = []
    lengths: dict[int, int] = {}
    count = regions = shard_count = 0
    shard_bytes = 0
    prefix = ('#include "x86.h"\n#include "seh.h"\n#include "intrinsics.h"\n' + LOCAL_ACCESSORS +
              "int sacred_ground_skip_build(uint32_t type);\n")
    declaration = ('struct GeneratedEntry { uint32_t address; void (*function)(X86*); };\n')

    shard_called: set[int] = set()

    def flush_shard() -> None:
        nonlocal shards, table, shard_count, shard_bytes
        if not shards:
            return

        tag = f"{module.name}_{shard_count:04d}"
        path = out / f"{tag}.c"
        calls = "".join(f"void region_{region:08x}(X86 *c);\n" for region in sorted(shard_called))
        calls += "".join(f"void {name}(X86 *c);\n" for name in sorted(tr.natives))
        shard_called.clear()
        text = prefix + calls + "\n".join(shards) + "\n" + declaration
        text += f"const struct GeneratedEntry entries_{tag}[] = {{\n"
        text += "\n".join(table) + "\n};\n"
        text += f"const unsigned count_{tag} = sizeof entries_{tag}/sizeof entries_{tag}[0];\n"
        unchanged_write(path, text)
        paths.append(str(path))
        shard_count += 1
        shards, table, shard_bytes = [], [], 0

    def flush_region() -> None:
        nonlocal instructions, regions, shard_bytes
        if not instructions:
            return

        start = instructions[0].addr
        end = instructions[-1].addr + lengths[instructions[-1].addr]
        fn = T.Function(start, f"region_{start:x}", end - start, instructions)
        fn.index = {ins.addr: i for i, ins in enumerate(instructions)}
        fn.fallthrough = [ins.addr + lengths[ins.addr] for ins in instructions]
        fn.seh_sites = T.seh_frame_sites(fn, image)
        fn.seh_restores = T.seh_restore_sites(fn)
        fn.seh_escapes = fn.pushed_continuations = fn.return_jumps = set()
        assert not tr.direct_region or tr.direct_region.get(start, start) == start, "direct-call region map out of step"
        linkage = "" if tr.direct_region else "static "
        # restrict: guest memory is written through g_mem (char aliasing), which otherwise makes the compiler reload
        # every guest register after every guest store. Nothing reaches the CPU context except through `c` while a
        # region runs: every helper that may change it is passed `c`.
        body = [f"{linkage}void region_{start:08x}(X86 *restrict c) {{", "RECOMP_LOCAL_MEMORY;"]
        tr.called = set()
        code, locals_ = promote.body(region_body(tr, fn, instructions, leaders, quarantined, hook_lines))
        # The guest registers and flags live in locals (promote.py), loaded once here before the switch.
        body += [locals_, "switch(c->eip) {"]
        body += [f"case 0x{instructions[i].addr:08x}u: goto L_{instructions[i].addr:08x};" for i in fn.entries]
        body += ["default: recomp_unknown_jump(c,c->eip); return;", "}"]
        # Every instruction gets a label; only those a goto names stay (an unused label is a warning in ISO mode).
        targets = set(_GOTO.findall("\n".join(body + code)))
        body += [line for line in code if not (m := _LABEL.fullmatch(line)) or m.group(1) in targets]
        body += ["}"]
        region_called = tr.called
        text = "\n".join(body)
        # Both C and address tables count toward the translation-unit budget.
        rows = [f"{{0x{ins.addr:08x}u, region_{start:08x}}}," for ins in instructions]
        size = len(text) + sum(map(len, rows))
        if shard_bytes + size > 250 * 1024 or len(shards) == 128:
            flush_shard()

        shards.append(text)
        shard_called.update(region_called)
        table.extend(rows)
        shard_bytes += size
        regions += 1
        instructions = []

    for rva, length in listing.instructions:
        # The export's bytes equal the player's file (its hash matched), so they are read from the file.
        code = original[rva:rva + length]
        assert len(code) == length, f"{module.filename}: instruction at rva {rva:#x} outside the image"
        pc = module.base + rva
        # Never split a recovered body at an arbitrary instruction: wait
        # for a byte-verified exported basic-block leader.
        if len(instructions) >= 128 and pc in leaders:
            flush_region()

        decoded = list(decoder.disasm(relocated[rva:rva + len(code)], pc))
        hook_site = resolution.hooks.get(original_base + rva) if resolution else None
        if hook_site and code != hook_site.original:
            raise SystemExit(f"{RESOLUTION_SITES}: hook {hook_site.address:#x}: exported instruction "
                             f"{code.hex()} is not {hook_site.original.hex()}")

        option_site = resolution.options.get(original_base + rva) if resolution else None
        if option_site and code != option_site.original:
            raise SystemExit(f"{RESOLUTION_SITES}: option {option_site.address:#x}: exported instruction "
                             f"{code.hex()} is not {option_site.original.hex()}")

        site = resolution.code.get(original_base + rva) if resolution else None
        if site and code != site.original:
            raise SystemExit(f"{RESOLUTION_SITES}: {site.address:#x}: exported instruction {code.hex()} "
                             f"is not {site.original.hex()}")

        if decoded and sum(ins.size for ins in decoded) == len(code):
            try:
                ins = normalized(image, decoded)
                if site:
                    try:
                        ops = R.rewrite(resolution, site, ins.ops, original, original_base)
                    except R.TableError as error:
                        raise SystemExit(f"{RESOLUTION_SITES}: {error}") from None

                    ins = T.Insn(ins.addr, ins.mnem, ins.rep, ops,
                                 f"{ins.addr:08x}  {ins.mnem} " + ",".join(ops) + "  [resolution cell]")
                    resolution_used.add(site.address)
            except T.TranslateError as error:
                quarantined.append({"pc": pc, "listing": decoded[0].mnemonic,
                                    "reason": str(error)})
                ins = T.Insn(pc, "UNQUALIFIED", None, [], f"{pc:08x}  UNQUALIFIED")
        else:
            ins = T.Insn(pc, "UNQUALIFIED", None, [], f"{pc:08x}  UNQUALIFIED")

        instructions.append(ins)
        lengths[pc] = len(code)
        count += 1

    flush_region()
    flush_shard()
    if resolution:
        failed = sorted({int(q["pc"]) for q in quarantined} & set(resolution.code))
        if failed:
            raise SystemExit(f"{RESOLUTION_SITES}: rewritten instructions not emitted: "
                             + ", ".join(f"{a:#x}" for a in failed))

        hooked = set(resolution.hooks) | set(resolution.options)
        missing = sorted((set(resolution.code) - resolution_used) | (hooked - hooks_used))
        if missing:
            raise SystemExit(f"{RESOLUTION_SITES}: sites not in the exported listing: "
                             + ", ".join(f"{a:#x}" for a in missing))

        path = out / "resolution.c"
        unchanged_write(path, R.emit_c(resolution, original_base))
        paths.append(str(path))

    receipt = {"module": module.filename, "sha256": listing.sha256,
               "assigned_base": module.base, "original_base": original_base,
               "instructions": count, "regions": regions, "shards": shard_count,
               "ir_sha256": ir_sha256,
               "blocks_ir_sha256": blocks_sha256,
               "annotations_sha256": hashlib.sha256(
                   (ROOT / "data/entry-targets.json").read_bytes()).hexdigest(),
               "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               "x87_runs_sha256": hashlib.sha256((Path(__file__).parent / "x87_fast.py").read_bytes()).hexdigest()
               if X87_RUNS else None,
               "translator_sha256": hashlib.sha256(
                   Path(T.__file__).read_bytes()).hexdigest(),
               "semantic_emitters_sha256": {
                   name: hashlib.sha256((Path(T.__file__).parent / name).read_bytes()).hexdigest()
                   for name in ("x87_emit.py", "simd_emit.py")},
               "helper_abi_sha256": hashlib.sha256(
                   b"".join((ROOT / name).read_bytes() for name in (
                       "third_party/recomp-kit/runtime/x86.h",
                       "third_party/recomp-kit/runtime/seh.h",
                       "third_party/recomp-kit/runtime/intrinsics.h",
                       "runtime/cpu/types.h", "runtime/cpu/x87.h", "runtime/cpu/x87_fast.h",
                       "runtime/cpu/simd.h"))).hexdigest(),
               "virtual_cpuid": json.loads((ROOT / "data/entry-cpuid.json").read_text()),
               "resolution_sites_sha256": hashlib.sha256(RESOLUTION_SITES.read_bytes()).hexdigest()
               if resolution else None,
               "quarantined": quarantined}
    unchanged_write(out / "receipt.json", json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({k: v for k, v in receipt.items() if k != "quarantined"}), flush=True)
    return paths


def main() -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    T.RESUMABLE_STACKS = T.DRIVER_CHECKPOINTS = True
    T.INTRINSIC_BODY = {}
    paths = [path for module in MODULES for path in generate(module)]
    # The game module (runtime/modules/game_module.h): the one exported symbol the runtime looks up.
    tags = [Path(path).stem for path in paths if Path(path).name != "resolution.c"]
    sacred_sha256 = hashlib.sha256((ASSETS / MODULES[0].filename).read_bytes()).hexdigest()
    registration = ['/* Generated by tools/lift/generate.py. */',
                    '#include "runtime/modules/game_module.h"']
    registration += [f"extern const struct GeneratedEntry entries_{tag}[];\nextern const unsigned count_{tag};"
                     for tag in tags]
    registration += ["static const struct SacredGeneratedTable tables[] = {"]
    registration += [f'    {{entries_{tag}, &count_{tag}, "{tag}"}},' for tag in tags]
    registration += ["};",
                     "static const struct SacredGameModule module = {",
                     "#ifdef SACRED_QA",
                     "    SACRED_GAME_MODULE_ABI, 1u,",
                     "#else",
                     "    SACRED_GAME_MODULE_ABI, 0u,",
                     "#endif",
                     f'    "{sacred_sha256}", {len(tags)}u, tables, &resolution_table}};',
                     "const struct SacredGameModule* sacred_game_module(void) { return &module; }"]
    unchanged_write(BUILD / "register.c", "\n".join(registration) + "\n")
    paths.append(str(BUILD / "register.c"))
    unchanged_write(BUILD / "sources.cmake", "set(SACRED_GENERATED\n" +
                    "\n".join(f'"{p}"' for p in paths) + "\n)\n")


if __name__ == "__main__":
    main()
