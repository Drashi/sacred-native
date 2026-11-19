"""Resolution-dependent sites of Sacred.exe: an opt-in screen size other than 1024x768.

The table (tools/lift/resolution_sites.json) is pinned to one executable hash. Each entry names an exact
instruction or data range by address and length (the bytes are read from the player's executable, pinned by its
hash), and an integer expression in W and H (the configured screen width and height) and F (the frame rate limit,
Settings.cfg FPS_LIMIT, 60 when unset) for its new value:

* ``code`` sites: the generator replaces one operand of the instruction with a load from a guest *cell*. An
  immediate operand (``push 0x400``) becomes ``[cell]``; an absolute memory operand (``fld [const]``) gets the
  cell's address instead. The expression must give the original value at W=1024, H=768, F=60, so with the modes off
  every cell holds the original constant and the game behaves exactly as before. The runtime writes every cell at start.
* ``data`` sites: initialized data the runtime overwrites only when the mode is on.

Cells live in the unused tail of the last .data page (after the section's virtual size, never referenced by the
image), so their addresses can be compiled into the generated code.
"""
from __future__ import annotations

import ast
import hashlib
import json
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Final

DEFAULT_W: Final = 1024
DEFAULT_H: Final = 768
DEFAULT_F: Final = 60  # the frame rate the game was built for (its limiter is called with 60)
# The menu canvas (D in expressions): in a frame wider than 4:3 and at least 768 high, the main menu and the character
# select are laid out on a canvas 768 * W / H wide (at most 1024 + 2 * MENU_EXTENSION_MAX) instead of 1024, their
# original 1024x768 in its middle, D pixels in from either side. runtime/modules/resolution.cpp has the same formula.
MENU_EXTENSION_MAX: Final = 406


def menu_extension(w: int, h: int) -> int:
    if h < DEFAULT_H or w * DEFAULT_H <= h * DEFAULT_W:
        return 0

    return min((DEFAULT_H * w // h - DEFAULT_W) // 2, MENU_EXTENSION_MAX)

TYPES: Final = {"i8": 1, "i16": 2, "i32": 4, "f32": 4, "f64": 8}
_WIDTH_WORD: Final = {1: "byte", 2: "word", 4: "dword", 8: "qword"}


class TableError(Exception):
    pass


# ---------------------------------------------------------------- expressions --

_ALLOWED: Final = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant, ast.Name, ast.Add, ast.Sub, ast.Mult,
                   ast.Div, ast.USub, ast.UAdd, ast.Load, ast.Call)


@dataclass(frozen=True)
class Expr:
    """Integer expression in W, H, F and D (menu canvas extension, menu_extension) with + - * / (C division:
    truncation toward zero) and hd(on, off): `on` with the mode enabled, `off` without it (a layout change that does
    not scale with the screen, e.g. a larger buffer). Fractional constants (an original float constant scaled by the
    screen) are allowed in floating-point cells; min(a, b) and max(a, b) clamp."""
    text: str
    tree: ast.Expression = field(compare=False, hash=False)

    @staticmethod
    def parse(text: str) -> "Expr":
        try:
            tree = ast.parse(text, mode="eval")
        except SyntaxError as error:
            raise TableError(f"bad expression {text!r}: {error}") from None

        for node in ast.walk(tree):
            if not isinstance(node, _ALLOWED):
                raise TableError(f"bad expression {text!r}: {type(node).__name__} not allowed")

            if isinstance(node, ast.Name) and node.id not in ("W", "H", "F", "D", "hd", "min", "max"):
                raise TableError(f"bad expression {text!r}: unknown name {node.id}")

            if isinstance(node, ast.Call) and (not isinstance(node.func, ast.Name) or
                                               node.func.id not in ("hd", "min", "max") or
                                               len(node.args) != 2 or node.keywords):
                raise TableError(f"bad expression {text!r}: only hd(on, off), min(a, b) and max(a, b) calls")

            if isinstance(node, ast.Name) and node.id in ("hd", "min", "max") and not any(
                    isinstance(c, ast.Call) and c.func is node for c in ast.walk(tree)):
                raise TableError(f"bad expression {text!r}: {node.id} is a function")

            if isinstance(node, ast.Constant) and (not isinstance(node.value, (int, float)) or
                                                   isinstance(node.value, bool)):
                raise TableError(f"bad expression {text!r}: only number constants")

        return Expr(" ".join(text.split()), tree)

    def value(self, w: int, h: int, hd: bool | None = None, floating: bool = False, f: int = DEFAULT_F) -> int | float:
        """Value for a w x h screen and a frame rate limit f; the mode counts as enabled unless w x h is the original
        size (or `hd` says)."""
        enabled = (w, h) != (DEFAULT_W, DEFAULT_H) if hd is None else hd

        def ev(node: ast.AST) -> int:
            if isinstance(node, ast.Expression):
                return ev(node.body)

            if isinstance(node, ast.Call):
                if node.func.id == "min":
                    return min(ev(node.args[0]), ev(node.args[1]))

                if node.func.id == "max":
                    return max(ev(node.args[0]), ev(node.args[1]))

                return ev(node.args[0]) if enabled else ev(node.args[1])

            if isinstance(node, ast.Constant):
                if isinstance(node.value, float) and not floating:
                    raise TableError(f"{self.text!r}: a fractional constant needs a floating-point cell")

                return float(node.value) if floating else node.value

            if isinstance(node, ast.Name):
                v = {"W": w, "H": h, "F": f, "D": menu_extension(w, h)}[node.id]
                return float(v) if floating else v

            if isinstance(node, ast.UnaryOp):
                return -ev(node.operand) if isinstance(node.op, ast.USub) else ev(node.operand)

            a, b = ev(node.left), ev(node.right)
            if isinstance(node.op, ast.Add):
                return a + b

            if isinstance(node.op, ast.Sub):
                return a - b

            if isinstance(node.op, ast.Mult):
                return a * b

            if b == 0:
                raise TableError(f"division by zero in {self.text!r}")

            if floating:
                return a / b

            q = abs(a) // abs(b)
            return q if (a >= 0) == (b >= 0) else -q

        return ev(self.tree)

    @property
    def uses_f(self) -> bool:
        """The value depends on the frame rate limit (the runtime may rewrite such a cell while the game runs)."""
        return any(isinstance(node, ast.Name) and node.id == "F" for node in ast.walk(self.tree))

    def c(self, floating: bool = False) -> str:
        """C expression in int64_t, or in double for a floating-point cell (then / is real division)."""
        cast = "(double)" if floating else "(int64_t)"

        def em(node: ast.AST) -> str:
            if isinstance(node, ast.Expression):
                return em(node.body)

            if isinstance(node, ast.Call):
                a, b = em(node.args[0]), em(node.args[1])
                if node.func.id == "min":
                    return f"({a} < {b} ? {a} : {b})"

                if node.func.id == "max":
                    return f"({a} > {b} ? {a} : {b})"

                return f"(hd ? {a} : {b})"

            if isinstance(node, ast.Constant):
                return f"{cast}{node.value!r}"

            if isinstance(node, ast.Name):
                return f"{cast}{node.id.lower()}"

            if isinstance(node, ast.UnaryOp):
                return f"({'-' if isinstance(node.op, ast.USub) else '+'}{em(node.operand)})"

            op = {ast.Add: "+", ast.Sub: "-", ast.Mult: "*", ast.Div: "/"}[type(node.op)]
            return f"({em(node.left)} {op} {em(node.right)})"

        return em(self.tree)


def encode(kind: str, value: int) -> bytes:
    """Little-endian bytes of a typed value (integers wrap to the type width)."""
    if kind == "f32":
        return struct.pack("<f", float(value))

    if kind == "f64":
        return struct.pack("<d", float(value))

    size = TYPES[kind]
    return (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little")


# --------------------------------------------------------------------- table --


@dataclass(frozen=True)
class Cell:
    address: int
    kind: str
    expr: Expr


@dataclass(frozen=True)
class CodeSite:
    address: int
    original: bytes
    operand: int
    kind: str
    expr: Expr
    note: str
    mode: str = "value"


@dataclass(frozen=True)
class DataSite:
    address: int
    original: bytes
    kind: str
    expr: Expr
    note: str


# Guest register file order of the generated code (c->r[n]).
REGISTERS = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")


@dataclass(frozen=True)
class Adjust:
    """One guest variable moved by a cell value: variable += cell (i32, i16 or f32_add), variable |= cell (or: flags
    that several hooks may raise in one frame, idempotent), or variable *= cell (f32_scale); kind "reg" moves the
    register named by `base` itself; kinds "save" and "restore" copy the 32-bit variable to and from a scratch cell
    of its own (a value changed only around one call: save, adjust, call, restore restores it bit for bit).
    With a `base` register the variable is at register + address (an outgoing call argument at ESP, a field of the
    object a register points at), otherwise at the absolute address."""
    address: int
    kind: str
    cell: int
    base: str | None = None
    slot: int = 0  # f32_carry: the scratch cell that carries the dropped fraction to the next run

    @property
    def esp(self) -> bool:
        return self.base == "esp"


@dataclass(frozen=True)
class Hook:
    """With the mode on, after the instruction at `address` (original bytes checked) has executed, add each
    adjustment's cell value to its guest variable. Used where a layout is computed once into a table."""
    address: int
    original: bytes
    adjusts: tuple[Adjust, ...]
    note: str
    before: bool = False
    # Optional (register, offset, value): run only when the 32-bit word at register + offset equals value, e.g. the
    # vtable of the object a shared method was called on. Without a register the offset is an absolute address.
    # only_if: (base register or None, address, accepted values, width 16 or 32)
    guard: tuple[str | None, int, tuple[int, ...], int] | None = None
    # "hd": with the screen mode on (default); "fps": with a frame rate limit other than 60 (the F cell).
    mode: str = "hd"


@dataclass(frozen=True)
class Option:
    """A Settings.cfg switch (any value but 0 turns it on): after the instruction at `address` (original bytes
    checked) has executed, the 32-bit `register` is set to `value` while the switch is on, or with `base` the 32-bit
    word at base register + `offset` (e.g. the argument a push just stored at ESP). Its cell holds 1 or 0 and is
    written at every start, independent of the screen size."""
    setting: str
    address: int
    original: bytes
    register: str
    value: int
    cell: int
    note: str
    offset: int | None = None
    # Return addresses (the word at ESP, for a hook on a function's last instruction before RET) of callers that
    # keep the original result.
    skip_returns: tuple[int, ...] = ()


@dataclass(frozen=True)
class OptionWrite:
    """Image data (original bytes checked) the runtime replaces while a Settings.cfg switch is on."""
    setting: str
    address: int
    original: bytes
    value: bytes
    note: str


@dataclass
class Table:
    sha256: str
    cell_base: int
    cell_end: int
    code: dict[int, CodeSite]
    data: list[DataSite]
    cells: list[Cell]
    cell_of: dict[tuple[str, str], int]
    hooks: dict[int, Hook] = field(default_factory=dict)
    options: dict[int, Option] = field(default_factory=dict)
    # Guest word in the cell area that hooks raise while a screen designed for 1024x768 is drawn: the runtime then
    # presents that frame's 1024x768 corner centred and maps the mouse the same way, and clears it (0 = none).
    inset: int = 0
    # Bytes the runtime writes into the cell area at every start (an option's replacement string table).
    blobs: list[tuple[int, bytes]] = field(default_factory=list)
    option_writes: list["OptionWrite"] = field(default_factory=list)


def _int(value: str | int, what: str) -> int:
    try:
        return value if isinstance(value, int) else int(value, 0)
    except ValueError:
        raise TableError(f"bad {what} {value!r}") from None


def _bytes(text: str, what: str) -> bytes:
    try:
        data = bytes.fromhex(text)
    except ValueError:
        raise TableError(f"bad bytes for {what}: {text!r}") from None

    if not data:
        raise TableError(f"empty bytes for {what}")

    return data


def load(path: Path, image: bytes | None = None, image_base: int = 0) -> Table:
    """The table; `image` (the mapped executable at `image_base`) supplies the original bytes of entries that give
    a length."""
    raw = json.loads(path.read_text())

    def source(entry: dict) -> bytes:
        address = _int(entry["address"], "address")
        if "bytes" in entry:
            return _bytes(entry["bytes"], f"{address:#x}")

        length = _int(entry.get("length", 0), "length")
        if length <= 0:
            raise TableError(f"{address:#x}: needs a length")

        if image is None:
            return bytes(length)

        rva = address - image_base
        if not 0 <= rva <= len(image) - length:
            raise TableError(f"{address:#x}: outside the image")

        return image[rva:rva + length]

    cell_base, cell_end = _int(raw["cell_base"], "cell_base"), _int(raw["cell_end"], "cell_end")
    if cell_base % 8 or cell_end <= cell_base:
        raise TableError("cell area must be 8-byte aligned and non-empty")

    table = Table(raw["sha256"].lower(), cell_base, cell_end, {}, [], [], {})
    next_cell = cell_base
    cell_limit = cell_end
    if "inset" in raw:
        table.inset = _int(raw["inset"], "inset")
        if table.inset % 8 or not cell_base <= table.inset < cell_end:
            raise TableError("inset word must be an 8-byte aligned slot of the cell area")

        cell_limit = table.inset

    def cell(kind: str, expr: Expr) -> int:
        nonlocal next_cell
        key = (kind, expr.text)
        if key not in table.cell_of:
            if next_cell + 8 > cell_limit:
                raise TableError("cell area exhausted")

            table.cell_of[key] = next_cell
            table.cells.append(Cell(next_cell, kind, expr))
            next_cell += 8

        return table.cell_of[key]

    def _slot(name: str) -> int:
        # A scratch word for "save"/"restore" adjustments, one per name, never shared with a value cell.
        nonlocal next_cell
        key = ("slot", name)
        if key not in table.cell_of:
            if next_cell + 8 > cell_limit:
                raise TableError("cell area exhausted")

            table.cell_of[key] = next_cell
            table.cells.append(Cell(next_cell, "i32", Expr.parse("0")))
            next_cell += 8

        return table.cell_of[key]

    for entry in raw.get("code", []):
        address = _int(entry["address"], "address")
        kind = entry.get("type", "i32")
        if kind not in TYPES:
            raise TableError(f"{address:#x}: unknown type {kind}")

        if address in table.code:
            raise TableError(f"{address:#x}: duplicate code site")

        mode = entry.get("mode", "value")
        if mode not in ("value", "disp", "hd_read"):
            raise TableError(f"{address:#x}: unknown mode {mode}")

        site = CodeSite(address, source(entry), int(entry.get("operand", 0)), kind,
                        Expr.parse(entry["value"]), entry.get("note", ""), mode)
        table.code[address] = site
        cell(kind, site.expr)
        if mode == "hd_read":
            cell("i32", HD_FLAG)

    for entry in raw.get("hooks", []):
        address = _int(entry["address"], "address")
        if address in table.hooks:
            raise TableError(f"{address:#x}: duplicate hook")

        adjusts = []
        for item in entry["adjust"]:
            kind = item.get("type", "i32")
            if kind in ("save", "restore"):
                base = item.get("base", "absolute")
                if base != "absolute" and base not in REGISTERS:
                    raise TableError(f"{address:#x}: adjustment base is absolute or a 32-bit register")

                adjusts.append(Adjust(_int(item["address"], "address"), kind, _slot(str(item["slot"])),
                                      None if base == "absolute" else base))
                continue

            if kind == "f32_carry":
                # A float that is truncated to whole units later: scaled by the cell and rounded down to a whole
                # number, the dropped fraction carried into the next run (scratch slot), so nothing is lost per run.
                base = item.get("base", "absolute")
                if base != "absolute" and base not in REGISTERS:
                    raise TableError(f"{address:#x}: adjustment base is absolute or a 32-bit register")

                adjusts.append(Adjust(_int(item["address"], "address"), kind,
                                      cell("f32", Expr.parse(item["value"])),
                                      None if base == "absolute" else base, _slot(str(item["slot"]))))
                continue

            if kind not in ("i16", "i32", "or", "f32_scale", "f32_add"):
                raise TableError(f"{address:#x}: hook adjustments are i16, i32, or, f32_scale, f32_add, save or "
                                 "restore")

            expr = Expr.parse(item["value"])
            if kind == "f32_scale":
                # A float multiplied by the cell: the identity at the original size, bit for bit.
                if encode("f32", expr.value(DEFAULT_W, DEFAULT_H, floating=True)) != encode("f32", 1.0):
                    raise TableError(f"{address:#x}: scale {expr.text} is not 1 at 1024x768")
            elif kind == "f32_add":
                if expr.value(DEFAULT_W, DEFAULT_H, floating=True) != 0:
                    raise TableError(f"{address:#x}: adjustment {expr.text} is not 0 at 1024x768")
            elif expr.value(DEFAULT_W, DEFAULT_H) != 0:
                raise TableError(f"{address:#x}: adjustment {expr.text} is not 0 at 1024x768")

            if "register" in item:
                # The register itself (not memory) moved by the cell value.
                if item["register"] not in REGISTERS or item["register"] == "esp" or kind != "i32" or \
                        "address" in item or "base" in item:
                    raise TableError(f"{address:#x}: a register adjustment names one 32-bit register, i32")

                adjusts.append(Adjust(0, "reg", cell("i32", expr), item["register"]))
                continue

            base = item.get("base", "absolute")
            if base != "absolute" and base not in REGISTERS:
                raise TableError(f"{address:#x}: adjustment base is absolute or a 32-bit register")

            adjusts.append(Adjust(_int(item["address"], "address"), kind,
                                  cell("f32" if kind.startswith("f32") else "i32", expr),
                                  None if base == "absolute" else base))

        mode = entry.get("mode", "hd")
        if mode not in ("hd", "fps"):
            raise TableError(f"{address:#x}: hook mode is hd or fps")

        if mode != "fps" and any(a.kind == "f32_carry" for a in adjusts):
            raise TableError(f"{address:#x}: f32_carry adjustments go in fps hooks")

        cell("i32", HD_FLAG if mode == "hd" else FPS_VALUE)
        when = entry.get("when", "after")
        if when not in ("before", "after"):
            raise TableError(f"{address:#x}: hook runs before or after its instruction")

        guard = None
        if "only_if" in entry:
            item = entry["only_if"]
            if "base" in item and item["base"] not in REGISTERS:
                raise TableError(f"{address:#x}: hook condition base is a 32-bit register")

            if "base" not in item and "address" not in item:
                raise TableError(f"{address:#x}: hook condition needs a register base or an absolute address")

            equals = item["equals"] if isinstance(item["equals"], list) else [item["equals"]]
            width = {"i32": 32, "i16": 16}.get(item.get("type", "i32"))
            if width is None or not equals:
                raise TableError(f"{address:#x}: hook condition is i32 or i16 and equals a value or a list of values")

            guard = (item.get("base"), _int(item.get("address", 0), "address"),
                     tuple(_int(v, "equals") for v in equals), width)

        table.hooks[address] = Hook(address, source(entry), tuple(adjusts),
                                    entry.get("note", ""), when == "before", guard, mode)

    switch_cells: dict[str, int] = {}
    for entry in raw.get("options", []):
        address = _int(entry["address"], "address")
        if address in table.options or address in table.hooks:
            raise TableError(f"{address:#x}: duplicate hook or option")

        setting = str(entry["setting"])
        if not setting or not all(c.isupper() or c.isdigit() or c == "_" for c in setting):
            raise TableError(f"{address:#x}: setting {setting!r} is not an upper-case Settings.cfg key")

        offset = None
        if setting not in switch_cells:
            switch_cells[setting] = _slot("option " + setting)

        if "text" in entry:
            # A string in the image (original bytes checked) replaced while the switch is on; a shorter text is
            # padded with NULs, so code that copies the literal by fixed-size moves stays in bounds.
            original = source(entry)
            text = str(entry["text"]).encode("ascii")
            if len(text) >= len(original) + (original[-1:] != b"\0"):
                raise TableError(f"{address:#x}: option text {entry['text']!r} does not fit the original string")

            table.option_writes.append(OptionWrite(setting, address, original, text.ljust(len(original), b"\0"),
                                                   entry.get("note", "")))
            continue

        if "table" in entry:
            # A pointer table in the image (address, original bytes checked) whose entries point at these strings
            # while the switch is on; the strings live in the cell area, written at every start.
            names = entry["table"]
            original = source(entry)
            if not names or not all(isinstance(n, str) and n.isascii() and n for n in names) or \
                    len(original) != 4 * len(names):
                raise TableError(f"{address:#x}: an option table has one string per original pointer")

            size = (sum(len(n) + 1 for n in names) + 7) & ~7
            if next_cell + size > cell_limit:
                raise TableError("cell area exhausted")

            text, words, at = b"", [], next_cell
            for n in names:
                words.append(at)
                text += n.encode() + b"\0"
                at += len(n) + 1

            table.blobs.append((next_cell, text + bytes(size - len(text))))
            next_cell += size
            table.option_writes.append(OptionWrite(setting, address, original,
                                                   b"".join(w.to_bytes(4, "little") for w in words),
                                                   entry.get("note", "")))
            continue

        if "base" in entry:
            register = entry["base"]
            if register not in REGISTERS or "register" in entry:
                raise TableError(f"{address:#x}: option base is a 32-bit register, without a register target")

            offset = _int(entry.get("offset", 0), "offset")
        else:
            register = entry["register"]
            if register not in REGISTERS or register == "esp":
                raise TableError(f"{address:#x}: option register is a 32-bit register other than esp")

        if setting not in switch_cells:
            switch_cells[setting] = _slot("option " + setting)

        table.options[address] = Option(setting, address, source(entry), register,
                                        _int(entry["value"], "value") & 0xFFFFFFFF, switch_cells[setting],
                                        entry.get("note", ""), offset,
                                        tuple(_int(a, "return address") for a in entry.get("skip_returns", [])))

    for entry in raw.get("data", []):
        address = _int(entry["address"], "address")
        kind = entry.get("type", "i32")
        if kind not in TYPES:
            raise TableError(f"{address:#x}: unknown type {kind}")

        original = source(entry)
        if len(original) != TYPES[kind]:
            raise TableError(f"{address:#x}: {kind} data site needs {TYPES[kind]} original bytes")

        table.data.append(DataSite(address, original, kind, Expr.parse(entry["value"]), entry.get("note", "")))

    spans = sorted((d.address, d.address + len(d.original)) for d in table.data)
    for (a, b), (c, _) in zip(spans, spans[1:]):
        if c < b:
            raise TableError(f"overlapping data sites at {a:#x} and {c:#x}")

    return table


def check_image(table: Table, raw: bytes, image: bytes, base: int) -> None:
    """Executable hash, original data bytes and an untouched cell area (never referenced by the image)."""
    if hashlib.sha256(raw).hexdigest() != table.sha256:
        raise TableError("resolution table is pinned to another executable")

    for site in table.data:
        rva = site.address - base
        if image[rva:rva + len(site.original)] != site.original:
            raise TableError(f"{site.address:#x}: data bytes differ from the table")

        default = site.expr.value(DEFAULT_W, DEFAULT_H, floating=site.kind.startswith("f"))
        if encode(site.kind, default) != site.original:
            raise TableError(f"{site.address:#x}: {site.expr.text} is not the original value at 1024x768")

    for write in table.option_writes:
        rva = write.address - base
        if image[rva:rva + len(write.original)] != write.original:
            raise TableError(f"{write.address:#x}: option table bytes differ from the table")

    if any(image[table.cell_base - base:table.cell_end - base]):
        raise TableError("cell area is not zero in the image")

    for high in range(table.cell_base >> 16, ((table.cell_end - 1) >> 16) + 1):
        needle = high.to_bytes(2, "little")
        at = raw.find(needle, 2)
        while at != -1:
            value = int.from_bytes(raw[at - 2:at + 2], "little")
            if table.cell_base <= value < table.cell_end:
                raise TableError(f"image refers to the cell area at file offset {at - 2:#x}")

            at = raw.find(needle, at + 1)


def _decode(kind: str, data: bytes) -> int | float:
    if kind == "f32":
        return struct.unpack("<f", data)[0]

    if kind == "f64":
        return struct.unpack("<d", data)[0]

    return int.from_bytes(data, "little")


def _imm(text: str) -> int | None:
    t = text.strip().lower()
    neg = t.startswith("-")
    t = t.lstrip("-")
    if not t.startswith("0x"):
        return None

    try:
        v = int(t, 16)
    except ValueError:
        return None

    return -v if neg else v


DISP_MARK: Final = "@cell"
HD_READ_MARK: Final = "@hdread"
HD_FLAG: Final = Expr.parse("hd(1, 0)")
FPS_VALUE: Final = Expr.parse("F")


def rewrite(table: Table, site: CodeSite, ops: list[str], image: bytes, base: int) -> list[str]:
    """Operands of the site's instruction with one of them redirected to the site's cell.

    mode "value" (default): an immediate becomes [cell]; an absolute memory operand gets the cell's address.
    mode "disp": a memory operand with base/index registers keeps them, its displacement is read from the cell
    (the operand text gets a DISP_MARK suffix that generate.py turns into the address expression)."""
    if site.operand >= len(ops):
        raise TableError(f"{site.address:#x}: no operand {site.operand}")

    old = ops[site.operand]
    size = TYPES[site.kind]
    cell = table.cell_of[(site.kind, site.expr.text)]
    default = site.expr.value(DEFAULT_W, DEFAULT_H, floating=site.kind.startswith("f"))
    word = old.split(" ptr ")[0].lower() if " ptr " in old else None
    inner = old[old.find("[") + 1:old.rfind("]")] if "[" in old else ""
    if site.mode == "hd_read":
        if "[" not in old or ":" in old:
            raise TableError(f"{site.address:#x}: operand {old!r} is not a memory operand")

        if {"byte": 1, "word": 2, "dword": 4, "float": 4, "qword": 8, "double": 8}.get(word) != size:
            raise TableError(f"{site.address:#x}: operand {old!r} is not {size} bytes")

        flag = table.cell_of[("i32", HD_FLAG.text)]
        new = old + f"{HD_READ_MARK}0x{flag:x},0x{cell:x}"
    elif site.mode == "disp":
        if site.kind != "i32":
            raise TableError(f"{site.address:#x}: a displacement cell is i32")

        terms = [t.strip() for t in inner.replace("-", "+-").split("+") if t.strip()]
        disp = [t for t in terms if _imm(t) is not None]
        regs = [t for t in terms if _imm(t) is None]
        if "[" not in old or ":" in old or len(disp) != 1 or not regs:
            raise TableError(f"{site.address:#x}: operand {old!r} has no register-based displacement")

        if _imm(disp[0]) != default:
            raise TableError(f"{site.address:#x}: {site.expr.text} gives {default:#x} at 1024x768, "
                             f"the displacement is {disp[0]}")

        new = old + f"{DISP_MARK}0x{cell:x}"
    elif (imm := _imm(old)) is not None:
        mask = (1 << (8 * size)) - 1
        if site.kind == "f64":
            raise TableError(f"{site.address:#x}: no 8-byte immediates")

        expected = int.from_bytes(encode(site.kind, default), "little")
        if (imm & mask) != expected:
            raise TableError(f"{site.address:#x}: {site.expr.text} gives {expected:#x} at 1024x768, "
                             f"the instruction has {imm & mask:#x}")

        new = f"{_WIDTH_WORD[size]} ptr [0x{cell:x}]"
    else:
        address = _imm(inner)
        if address is None or word is None or ":" in old:
            raise TableError(f"{site.address:#x}: operand {old!r} is neither an immediate nor an absolute memory "
                             "operand")

        if {"byte": 1, "word": 2, "dword": 4, "float": 4, "qword": 8, "double": 8}.get(word) != size:
            raise TableError(f"{site.address:#x}: operand {old!r} is not {size} bytes")

        current = image[address - base:address - base + size]
        if current != encode(site.kind, default):
            raise TableError(f"{site.address:#x}: {site.expr.text} at 1024x768 differs from [{address:#x}]")

        new = f"{word} ptr [0x{cell:x}]"

    result = list(ops)
    result[site.operand] = new
    return result


def hook_c(table: Table, hook: Hook) -> list[str]:
    """C statements run after the hook instruction (generated region code; rd32/wr32/rd16/wr16 of x86.h)."""
    if hook.mode == "fps":
        condition = f"rd32({table.cell_of[('i32', FPS_VALUE.text)]:#x}u) != {DEFAULT_F}u"
    else:
        condition = f"rd32({table.cell_of[('i32', HD_FLAG.text)]:#x}u)"

    if hook.guard:
        register, offset, values, width = hook.guard
        where = f"c->r[{REGISTERS.index(register)}] + {offset:#x}u" if register else f"{offset:#x}u"
        read = f"rd{width}({where})"
        condition += (f" && {read} == {values[0]:#x}u" if len(values) == 1 else
                      " && (" + " || ".join(f"{read} == {v:#x}u" for v in values) + ")")

    lines = [f"if ({condition}) {{  /* resolution hook {hook.address:#010x} */"]
    for a in hook.adjusts:
        if a.kind == "reg":
            lines.append(f"    c->r[{REGISTERS.index(a.base)}] += rd32({a.cell:#x}u);")
            continue

        where = f"c->r[{REGISTERS.index(a.base)}] + {a.address:#x}u" if a.base else f"{a.address:#x}u"
        if a.kind == "save":
            lines.append(f"    wr32({a.cell:#x}u, rd32({where}));")
            continue

        if a.kind == "restore":
            lines.append(f"    wr32({where}, rd32({a.cell:#x}u));")
            continue

        if a.kind == "f32_carry":
            lines.append(f"    {{ uint32_t u_ = rd32({where}), k_ = rd32({a.cell:#x}u), q_ = rd32({a.slot:#x}u); "
                         f"float f_, s_, r_; memcpy(&f_, &u_, 4); memcpy(&s_, &k_, 4); memcpy(&r_, &q_, 4); "
                         f"double v_ = (double)f_ * s_ + r_; double n_ = v_ > 0 ? (double)(int64_t)v_ : 0; "
                         f"r_ = (float)(v_ - n_); f_ = (float)n_; memcpy(&u_, &f_, 4); memcpy(&q_, &r_, 4); "
                         f"wr32({where}, u_); wr32({a.slot:#x}u, q_); }}")
            continue

        if a.kind in ("f32_scale", "f32_add"):
            lines.append(f"    {{ uint32_t u_ = rd32({where}), k_ = rd32({a.cell:#x}u); float f_, s_; "
                         f"memcpy(&f_, &u_, 4); memcpy(&s_, &k_, 4); f_ {'*' if a.kind == 'f32_scale' else '+'}= s_; "
                         f"memcpy(&u_, &f_, 4); "
                         f"wr32({where}, u_); }}")
        elif a.kind == "i32":
            lines.append(f"    wr32({where}, rd32({where}) + rd32({a.cell:#x}u));")
        elif a.kind == "or":
            lines.append(f"    wr32({where}, rd32({where}) | rd32({a.cell:#x}u));")
        else:
            lines.append(f"    wr16({where}, (uint16_t)(rd16({where}) + rd32({a.cell:#x}u)));")

    lines.append("}")
    return lines


def _c_bytes(data: bytes) -> str:
    return "".join(f"\\x{x:02x}" for x in data)


def option_c(option: Option) -> list[str]:
    """C statements run after the option's instruction (generated region code)."""
    skips = "".join(f" && rd32(c->r[4]) != {a:#x}u" for a in option.skip_returns)
    return [f"if (rd32({option.cell:#x}u){skips}) {{  /* {option.setting} option {option.address:#010x} */",
            f"    c->r[{REGISTERS.index(option.register)}] = {option.value:#x}u;" if option.offset is None else
            f"    wr32(c->r[{REGISTERS.index(option.register)}] + {option.offset:#x}u, {option.value:#x}u);",
            "}"]


def emit_c(table: Table, base: int) -> str:
    """Runtime table: cells (written at start) and data sites (written only with the mode on)."""

    def value_case(index: int, kind: str, expr: Expr) -> str:
        if kind == "f32":
            return (f"case {index}u: {{ float v_ = (float){expr.c(True)}; uint32_t u_; memcpy(&u_, &v_, 4); "
                    f"return u_; }}")

        if kind == "f64":
            return (f"case {index}u: {{ double v_ = (double){expr.c(True)}; uint64_t u_; memcpy(&u_, &v_, 8); "
                    f"return u_; }}")

        return f"case {index}u: return (uint64_t){expr.c()};"

    rows, cases = [], []
    switches = sorted({(name, table.cell_of[("slot", "option " + name)])
                       for name in {o.setting for o in table.options.values()} |
                       {w.setting for w in table.option_writes}})
    option_cells = {cell for _, cell in switches}
    entries = [(c.address, c.kind, c.expr, 1) for c in table.cells if c.address not in option_cells] + \
              [(d.address, d.kind, d.expr, 0) for d in table.data]
    for index, (address, kind, expr, always) in enumerate(entries):
        rows.append(f"    {{0x{address:08x}u, {TYPES[kind]}u, {always}u, {int(expr.uses_f)}u}},  "
                    f"/* {kind} {expr.text} */")
        cases.append("    " + value_case(index, kind, expr))

    lines = [
        "/* Generated by tools/lift/generate.py from tools/lift/resolution_sites.json. */",
        "#include <stdint.h>",
        "#include <string.h>",
        '#include "runtime/modules/resolution_table.h"',
        f'const char resolution_table_sha256[] = "{table.sha256}";',
        f"const unsigned resolution_write_count = {len(entries)}u;",
        f"const uint32_t resolution_inset_address = 0x{table.inset:08x}u;",
        f"const unsigned resolution_blob_count = {len(table.blobs)}u;",
        "const struct ResolutionBlob resolution_blobs[] = {",
        *([f'    {{0x{a:08x}u, {len(b)}u, "{_c_bytes(b)}"}},' for a, b in table.blobs]
          or ['    {0u, 0u, ""}']),
        "};",
        f"const unsigned resolution_option_write_count = {len(table.option_writes)}u;",
        "const struct ResolutionOptionWrite resolution_option_writes[] = {",
        *([f'    {{"{w.setting}", 0x{w.address:08x}u, {len(w.value)}u, "{_c_bytes(w.value)}"}},'
           for w in table.option_writes] or ['    {"", 0u, 0u, ""}']),
        "};",
        f"const unsigned resolution_option_count = {len(switches)}u;",
        "const struct ResolutionOption resolution_options[] = {",
        *([f'    {{"{name}", 0x{cell:08x}u}},' for name, cell in switches] or ['    {"", 0u}']),
        "};",
        "const struct ResolutionWrite resolution_writes[] = {",
        *(rows or ["    {0u, 0u, 0u, 0u}"]),
        "};",
        "uint64_t resolution_value(unsigned index, int32_t w, int32_t h, int32_t hd, int32_t f) {",
        f"    const int32_t d = h < {DEFAULT_H} || (int64_t)w * {DEFAULT_H} <= (int64_t)h * {DEFAULT_W} ? 0 :",
        f"        ({DEFAULT_H} * (int64_t)w / h - {DEFAULT_W}) / 2 < {MENU_EXTENSION_MAX} ? "
        f"(int32_t)(({DEFAULT_H} * (int64_t)w / h - {DEFAULT_W}) / 2) : {MENU_EXTENSION_MAX};",
        "    (void)w; (void)h; (void)hd; (void)f; (void)d;",
        "    switch (index) {",
        *cases,
        "    default: return 0u;",
        "    }",
        "}",
        f'const struct ResolutionTable resolution_table = {{"{table.sha256}", {len(entries)}u, 0x{table.inset:08x}u, '
        f"resolution_writes, {len(table.blobs)}u, resolution_blobs, {len(table.option_writes)}u, "
        "resolution_option_writes, "
        f"{len(switches)}u, resolution_options, resolution_value}};",
    ]
    return "\n".join(lines) + "\n"
