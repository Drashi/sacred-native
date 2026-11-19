"""Native-double x87 runs for the region generator (runtime/cpu/x87_fast.h describes the contract).

A run starts at a supported x87 instruction and extends over the following instructions of the same block while each
is a supported x87 instruction or an integer instruction that touches no x87 state and calls nothing (it is emitted
unchanged). The x87 stack is simulated at generation time: every register the run reads or writes has a position
relative to TOP at run entry, its value lives in a host double, and the run writes the changed registers back at its
end. The entry check (x87f_enter) and a conversion check of each register read on entry guard the whole run; inside,
an instruction whose double result could differ from the x87 one checks it and, on failure, writes the stack of the
moment back and continues in the per-instruction code at that instruction.

Exponent bounds: each value carries (lo, hi, q): it is zero or 2^lo <= |v| <= 2^hi, and a multiple of 2^q. While a
result provably stays a normal double (lo >= -1022, hi <= 1023) it is the x87 result and needs no check.
"""
from __future__ import annotations

from dataclasses import dataclass, field, replace
import copy

import translate as T

WIN = (-500, 500, -552)          # a checked value: zero or a normal double in [2^-500, 2^500)
F32 = (-126, 128, -149)          # a normal single
INT = {16: (0, 15, 0), 32: (0, 31, 0), 64: (0, 53, 0)}
ONE = (0, 0, 0)

_CONTROL = {"FNSTSW", "FSTSW", "FNSTCW", "FSTCW"}
_BINARY = {"FADD", "FSUB", "FSUBR", "FMUL", "FDIV", "FDIVR", "FADDP", "FSUBP", "FSUBRP", "FMULP", "FDIVP",
           "FDIVRP", "FIADD", "FISUB", "FISUBR", "FIMUL", "FIDIV", "FIDIVR"}
_COMPARE = {"FCOM", "FCOMP", "FCOMPP", "FUCOM", "FUCOMP", "FUCOMPP", "FCOMI", "FCOMIP", "FUCOMI", "FUCOMIP",
            "FICOM", "FICOMP", "FTST"}
SUPPORTED = (_CONTROL | _BINARY | _COMPARE |
             {"FLD", "FILD", "FLD1", "FLDZ", "FST", "FSTP", "FIST", "FISTP", "FCHS", "FABS", "FXCH", "FSQRT"})
# Instructions after which C1 holds a value the double code does not compute ("computed"), or zero ("clear").
_C1_NONE = {"FXCH", "FCHS", "FABS"} | _CONTROL


class Stop(Exception):
    """The instruction cannot join the run (unsupported form, stack fault, register span)."""


def safe(b: tuple[int, int, int]) -> bool:
    return b[0] >= -1022 and b[1] <= 1023


@dataclass
class Slot:
    var: str
    bounds: tuple[int, int, int]
    full: bool = True


@dataclass
class State:
    top: int = 0
    slots: dict[int, Slot] = field(default_factory=dict)
    dirty: set[int] = field(default_factory=set)
    need_full: set[int] = field(default_factory=set)
    need_empty: set[int] = field(default_factory=set)
    sw_dirty: bool = False
    dp: str | None = None        # C variable with the last x87 data pointer of the run
    ip: tuple[int, int] | None = None


def opcode_of(image, ins) -> int:
    at = ins.addr
    while image.rd8(at) in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0x9b):
        at += 1

    return ((image.rd8(at) & 7) << 8) | image.rd8(at + 1)


def first_opcode_byte(image, ins) -> int:
    at = ins.addr
    while image.rd8(at) in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0x9b):
        at += 1

    return image.rd8(at)


_FORBIDDEN = ("goto", "return", "c->eip", "fpu", "c->st", "x87", "mmx", "ftag", "fxch", "fdrop", "recomp_")


def inert(ins, emitted: list[str], needs_eip) -> bool:
    """An integer instruction the double code may run unchanged in the middle of a run."""
    if ins.mnem.startswith("F") or ins.mnem in ("WAIT", "FWAIT") or needs_eip(emitted):
        return False

    return not any(word in line.split("/*", 1)[0] for line in emitted for word in _FORBIDDEN)


class Compiler:
    def __init__(self, image, instructions, emitted, observed):
        self.image = image
        self.instructions = instructions
        self.emitted = emitted
        self.observed = observed   # instruction index -> its C1 is read (FNSTSW or the end of the run)
        self.start = None
        self.s = State()
        self.lines: list[str] = []
        self.n = 0

    # -- values ---------------------------------------------------------------------------------------------------
    def new(self, expr: str, ctype: str = "double") -> str:
        name = f"v{self.n}_"
        self.n += 1
        self.lines.append(f"const {ctype} {name} = {expr};")
        return name

    def span(self, p: int) -> None:
        used = set(self.s.slots) | self.s.need_full | self.s.need_empty | {p}
        if max(used) - min(used) > 7:
            raise Stop("register span")

    def read(self, i: int) -> Slot:
        p = self.s.top + i
        slot = self.s.slots.get(p)
        if slot is None:
            assert p >= 0
            self.span(p)
            self.s.need_full.add(p)
            slot = self.s.slots[p] = Slot(f"e{p}_", WIN)
        elif not slot.full:
            raise Stop("stack underflow")

        return slot

    def write(self, i: int, slot: Slot) -> None:
        p = self.s.top + i
        self.span(p)
        self.s.slots[p] = replace(slot, full=True)
        self.s.dirty.add(p)

    def push(self, slot: Slot) -> None:
        p = self.s.top - 1
        old = self.s.slots.get(p)
        if old is not None and old.full:
            raise Stop("stack overflow")

        self.span(p)
        if old is None:
            self.s.need_empty.add(p)

        self.s.slots[p] = replace(slot, full=True)
        self.s.dirty.add(p)
        self.s.top = p

    def pop(self) -> None:
        slot = self.read(0)
        self.s.slots[self.s.top] = replace(slot, full=False)
        self.s.dirty.add(self.s.top)
        self.s.top += 1

    # -- status ---------------------------------------------------------------------------------------------------
    def c1(self, k: int, expr: str) -> None:
        if self.observed.get(k):
            self.lines.append(f"sw_ = (uint16_t)((sw_ & ~0x200u) | (({expr}) ? 0x200u : 0u));"
                              if expr != "0" else "sw_ &= (uint16_t)~0x200u;")
            self.s.sw_dirty = True

    def deopt(self, k: int, before: State, condition: str) -> None:
        """Continue at instruction k in the per-instruction code, with the stack as it stood before k."""
        ins = self.instructions[k]
        has_memory = any(T.parse_operand(o).kind == "mem" for o in ins.ops if o.strip())
        dp = before.dp if not has_memory and before.dp is not None else None
        positions = sorted(before.dirty)
        # The run's first instruction resumes behind the double block (its label leads back into it).
        label = f"F_{ins.addr:08x}" if k == self.start else f"L_{ins.addr:08x}"
        target = f"X87F_COUNT(1); goto {label};"
        if not positions and before.top == 0 and not before.sw_dirty and dp is None:
            self.lines.append(f"if (RECOMP_UNLIKELY(!({condition}))) {{ {target} }}")
            return

        full = sum(1 << i for i, p in enumerate(positions) if before.slots[p].full)
        if positions:
            rel = ", ".join(str(p) for p in positions)
            values = ", ".join(before.slots[p].var for p in positions)
            spill = (f"static const int8_t p_[] = {{{rel}}}; const double s_[] = {{{values}}}; "
                     f"x87f_spill(c, t0_, {len(positions)}u, p_, s_, 0x{full:x}u, {before.top}, sw_, "
                     f"{int(dp is not None)}, {dp or '0'});")
        else:
            spill = (f"x87f_spill(c, t0_, 0u, 0, 0, 0u, {before.top}, sw_, {int(dp is not None)}, "
                     f"{dp or '0'});")

        self.lines.append(f"if (RECOMP_UNLIKELY(!({condition}))) {{ {spill} {target} }}")

    # -- instructions ---------------------------------------------------------------------------------------------
    def load(self, k: int, before: State, op) -> Slot:
        """A memory operand as a double (deoptimises on denormal, infinite or NaN input)."""
        address = self.address(op)
        if op.size == 32:
            bits = self.new(f"rd32({address})", "uint32_t")
            self.deopt(k, before, f"x87f_ok_f32({bits})")
            return Slot(self.new(f"x87f_f32({bits})"), F32)

        if op.size == 64:
            bits = self.new(f"rd64({address})", "uint64_t")
            self.deopt(k, before, f"x87f_ok_f64({bits})")
            return Slot(self.new(f"x87f_double({bits})"), WIN)

        raise Stop("memory operand size")

    def integer(self, op) -> Slot:
        address = self.address(op)
        if op.size not in (16, 32):
            raise Stop("integer operand size")

        return Slot(self.new(f"(double)(int{op.size}_t)rd{op.size}({address})"), INT[op.size])

    def address(self, op) -> str:
        """The operand address, kept for the data pointer."""
        name = self.new(T.addr_expr(op), "uint32_t")
        self.s.dp = name
        return name

    def binary(self, k: int, before: State, operation: str, a: Slot, b: Slot) -> Slot:
        symbol = {"ADD": "+", "SUB": "-", "MUL": "*", "DIV": "/"}[operation]
        r = self.new(f"{a.var} {symbol} {b.var}")
        (la, ha, qa), (lb, hb, qb) = a.bounds, b.bounds
        if operation in ("ADD", "SUB"):
            bounds = (min(qa, qb), max(ha, hb) + 1, min(qa, qb))
            check = f"x87f_ok_add({r})"
        elif operation == "MUL":
            bounds = (la + lb, ha + hb, min(qa + qb, la + lb - 52))
            check = f"x87f_ok_mul({r}, {a.var}, {b.var})"
        else:
            bounds, check = (-1023, 1024, -1074), f"x87f_ok_div({r}, {a.var}, {b.var})"

        if not safe(bounds):
            self.deopt(k, before, check)
            bounds = WIN

        if self.observed.get(k):
            if operation in ("ADD", "SUB"):
                y = b.var if operation == "ADD" else f"-{b.var}"
                self.c1(k, f"x87f_c1_add({a.var}, {y}, {r})")
            else:
                flag = f"c{k}_"
                self.lines.append(f"unsigned {flag};")
                helper = "x87f_c1_mul" if operation == "MUL" else "x87f_c1_div"
                self.deopt(k, before, f"{helper}({a.var}, {b.var}, {r}, &{flag})")
                self.c1(k, flag)

        return Slot(r, bounds)

    def step(self, k: int) -> None:
        ins = self.instructions[k]
        m = ins.mnem
        if m not in SUPPORTED:
            raise Stop(m)

        ops = [T.parse_operand(o) for o in ins.ops if o.strip()] if ins.ops else []
        before = copy.deepcopy(self.s)
        if m not in _CONTROL:
            self.s.ip = (ins.addr, opcode_of(self.image, ins))

        if m == "FLD":
            op = ops[0]
            if op.kind == "st":
                self.push(self.read(op.sti))
            else:
                self.push(self.load(k, before, op))

            self.c1(k, "0")
        elif m in ("FLD1", "FLDZ"):
            self.push(Slot("1.0" if m == "FLD1" else "0.0", ONE))
            self.c1(k, "0")
        elif m == "FILD":
            op = ops[0]
            if op.size == 64:
                address = self.address(op)
                value = self.new(f"(int64_t)rd64({address})", "int64_t")
                self.deopt(k, before, f"(uint64_t){value} + (UINT64_C(1) << 53) <= (UINT64_C(1) << 54)")
                self.push(Slot(self.new(f"(double){value}"), INT[64]))
            else:
                self.push(self.integer(op))

            self.c1(k, "0")
        elif m in ("FST", "FSTP"):
            op = ops[0]
            value = self.read(0)
            if op.kind == "st":
                self.write(op.sti, value)
            elif op.size == 32:
                address = self.address(op)
                single = self.new(f"(float){value.var}", "float")
                self.deopt(k, before, f"x87f_ok_st32({value.var}, {single})")
                self.lines.append(f"{{ uint32_t b_; memcpy(&b_, &{single}, 4); wr32({address}, b_); }}")
                self.c1(k, f"(double){single} != {value.var} && fabs((double){single}) > fabs({value.var})")
            elif op.size == 64:
                self.lines.append(f"wr64({self.address(op)}, x87f_bits({value.var}));")
                self.c1(k, "0")
            else:
                raise Stop("store size")

            if m == "FSTP":
                self.pop()
        elif m in ("FIST", "FISTP"):
            op = ops[0]
            limits = {16: ("-32768.0", "<= 32767.0"), 32: ("-2147483648.0", "<= 2147483647.0"),
                      64: ("-0x1p63", "< 0x1p63")}
            if op.size not in limits:
                raise Stop("integer store size")

            value = self.read(0)
            address = self.address(op)
            rounded = self.new(f"rint({value.var})")
            low, high = limits[op.size]
            self.deopt(k, before, f"{rounded} >= {low} && {rounded} {high}")
            self.lines.append(f"wr{op.size}({address}, (uint{op.size}_t)(int{op.size}_t){rounded});")
            self.c1(k, f"fabs({rounded}) > fabs({value.var})")
            if m == "FISTP":
                self.pop()
        elif m in _BINARY:
            base = m[:-1] if m.endswith("P") else m
            kind = base.replace("FI", "F")[:4]
            operation = {"FADD": "ADD", "FSUB": "SUB", "FMUL": "MUL", "FDIV": "DIV"}[kind]
            dest = 0
            if m.endswith("P"):
                dest = ops[0].sti if ops else 1
                source = self.read(0)
            elif ops[0].kind == "mem":
                source = self.integer(ops[0]) if m.startswith("FI") else self.load(k, before, ops[0])
            elif len(ops) == 2:
                dest = ops[0].sti
                source = self.read(ops[1].sti)
            elif first_opcode_byte(self.image, ins) == 0xdc:
                dest = ops[0].sti
                source = self.read(0)
            else:
                source = self.read(ops[0].sti)

            a, b = self.read(dest), source
            if base.endswith("R"):
                a, b = b, a

            self.write(dest, self.binary(k, before, operation, a, b))
            if m.endswith("P"):
                self.pop()
        elif m in _COMPARE:
            a = self.read(0)
            if m == "FTST":
                b = Slot("0.0", ONE)
            elif not ops:
                b = self.read(1)
            elif ops[-1].kind == "st":
                b = self.read(ops[-1].sti)
            elif m.startswith("FI"):
                b = self.integer(ops[-1])
            else:
                b = self.load(k, before, ops[-1])

            if "I" in m[2:]:
                self.lines.append(f"c->eflags_of = c->eflags_sf = c->eflags_af = c->eflags_pf = 0; "
                                  f"c->eflags_zf = {a.var} == {b.var}; c->eflags_cf = {a.var} < {b.var};")
                self.c1(k, "0")
            else:
                self.lines.append(f"sw_ = (uint16_t)((sw_ & ~0x4700u) | ({a.var} == {b.var} ? 0x4000u : 0u) | "
                                  f"({a.var} < {b.var} ? 0x100u : 0u));")
                self.s.sw_dirty = True

            for _ in range(2 if m.endswith("PP") else 1 if m.endswith("P") else 0):
                self.pop()
        elif m in ("FCHS", "FABS"):
            value = self.read(0)
            self.write(0, Slot(self.new(f"-{value.var}" if m == "FCHS" else f"fabs({value.var})"), value.bounds))
        elif m == "FXCH":
            i = ops[-1].sti if ops else 1
            a, b = self.read(0), self.read(i)
            self.write(0, b)
            self.write(i, a)
        elif m == "FSQRT":
            value = self.read(0)
            r = self.new(f"sqrt({value.var})")
            self.deopt(k, before, f"x87f_ok_sqrt({r})")
            if self.observed.get(k):
                flag = f"c{k}_"
                self.lines.append(f"unsigned {flag};")
                self.deopt(k, before, f"x87f_c1_sqrt({value.var}, {r}, &{flag})")
                self.c1(k, flag)

            self.write(0, Slot(r, WIN))
        elif m in ("FNSTSW", "FSTSW"):
            word = f"(uint16_t)((sw_ & ~0x3800u) | (((t0_ + {self.s.top & 7}u) & 7u) << 11))"
            op = ops[0]
            if op.kind == "reg":
                self.lines.append(f"c->r[0] = (c->r[0] & 0xffff0000u) | {word};")
            else:
                self.lines.append(f"wr16({T.addr_expr(op)}, {word});")
        elif m in ("FNSTCW", "FSTCW"):
            self.lines.append(f"wr16({T.addr_expr(ops[0])}, c->fpu_cw);")
        else:
            raise Stop(m)


def c1_kind(ins) -> str:
    """'observe' (reads C1), 'write' (sets it) or 'none'."""
    if ins.mnem in ("FNSTSW", "FSTSW"):
        return "observe"

    if not ins.mnem.startswith("F") or ins.mnem in _C1_NONE:
        return "none"

    if ins.mnem in ("FST", "FSTP") and T.parse_operand(ins.ops[0]).kind == "st":
        return "none"

    return "write"


def observed_writers(instructions: list) -> dict[int, bool]:
    result, pending = {}, True        # the end of the run observes the last writer
    for k in range(len(instructions) - 1, -1, -1):
        kind = c1_kind(instructions[k])
        if kind == "observe":
            pending = True
        elif kind == "write":
            result[k] = pending
            pending = False

    return result


def compile_run(image, instructions, emitted, start: int, limit: int, inert_at,
                exit_lines) -> tuple[int, list[str]] | None:
    """The double version of the run starting at `start` (instructions start..end, end < limit), or None."""
    # Pass 1: how far the run reaches, with every C1 observed (only the extent matters).
    probe = Compiler(image, instructions, emitted, {k: True for k in range(len(instructions))})
    probe.start = start
    end = start - 1
    for k in range(start, limit):
        if instructions[k].mnem.startswith("F"):
            state, lines, n = copy.deepcopy(probe.s), list(probe.lines), probe.n
            try:
                probe.step(k)
            except Stop:
                probe.s, probe.lines, probe.n = state, lines, n
                break

            end = k
        elif not inert_at(k):
            break

    run = instructions[start:end + 1]
    x87 = [ins for ins in run if ins.mnem.startswith("F")]
    if len(x87) < 2 or all(ins.mnem in _CONTROL | {"FXCH"} for ins in x87):
        return None

    # Pass 2: the real code, C1 computed only where it is read.
    observed = {start + k: v for k, v in observed_writers(run).items()}
    compiler = Compiler(image, instructions, emitted, observed)
    compiler.start = start
    body: list[str] = []
    for k in range(start, end + 1):
        if instructions[k].mnem.startswith("F"):
            compiler.step(k)
        else:
            compiler.lines += emitted[k]

    s = compiler.s
    mask = expect = 0
    for p in s.need_full:
        mask |= 2 << (2 * (p & 7))

    for p in s.need_empty:
        mask |= 3 << (2 * (p & 7))
        expect |= 3 << (2 * (p & 7))

    name = f"F_{instructions[start].addr:08x}"
    body.append(f"if (x87f_enter(c, 0x{mask:x}u, 0x{expect:x}u)) {{  /* x87 run to {instructions[end].addr:08x} */")
    body.append("const unsigned t0_ = c->fpu_top;")
    body.append("uint16_t sw_ = c->fpu_sw;")
    sw_at = len(body) - 1
    entry = sorted(s.need_full)
    if entry:
        body.append("double " + ", ".join(f"e{p}_" for p in entry) + ";")
        tests = " & ".join(f"x87f_in(c->st[(t0_ + {p}u) & 7u], &e{p}_)" for p in entry)
        body.append(f"if (RECOMP_UNLIKELY(!({tests}))) {{ X87F_COUNT(1); goto {name}; }}")

    body += compiler.lines
    for p in sorted(s.dirty):
        slot = s.slots[p]
        body.append(f"x87f_put(c, t0_ + {p & 7}u, {slot.var}, {int(slot.full)});")

    if s.top:
        body.append(f"c->fpu_top = (t0_ + {s.top & 7}u) & 7u;")

    if s.sw_dirty:
        body.append("c->fpu_sw = sw_;")

    if s.ip:
        body.append(f"c->fpu_ip = 0x{s.ip[0]:08x}u; c->fpu_opcode = 0x{s.ip[1]:x}; c->fpu_cs = c->cs;")

    if s.dp:
        body.append(f"c->fpu_dp = {s.dp}; c->fpu_ds = c->ds;")

    body.append("X87F_COUNT(2);")
    body += exit_lines(end)
    body += ["}", f"{name}: ;"]
    if not any("sw_" in line for k, line in enumerate(body) if k > sw_at):
        del body[sw_at]  # a run that neither reads nor sets the status word (unused variable)

    return end, body
