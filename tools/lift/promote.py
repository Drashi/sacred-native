"""Guest registers and flags of a region function in C locals.

The region code reads and writes the CPU context (c->r[], c->eflags_*) at every instruction. In locals the
compiler keeps them in host registers and drops dead flag computations. The context is brought up to date only
where something else can look at it: before a call that takes `c` and may read or change the context (a helper,
a direct call, a host call, a possible guest exception), after which the locals are loaded again, and before every
return. Only the locals the region writes are stored there, and only those it uses are loaded again.

Helpers that touch only the x87/MMX/SSE state, guest memory or the scheduling budget are called without that
(PURE); a few write the flags only (FLAGS_OUT) and are followed by a reload of the flags.
"""
import re
from typing import Final

_FLAG_NAMES: Final = ("cf", "zf", "sf", "of", "pf", "af", "df")
NAMES: Final = [f"r{i}_" for i in range(8)] + [f"f{f}_" for f in _FLAG_NAMES]
FIELD: Final = {**{f"r{i}_": f"c->r[{i}]" for i in range(8)}, **{f"f{f}_": f"c->eflags_{f}" for f in _FLAG_NAMES}}

# Read and write neither guest registers nor flags, and raise no guest exception (x87 exceptions are raised by
# x87_wait; a guest memory fault in any helper is fatal).
PURE: Final = frozenset("""
    recomp_source recomp_block recomp_tick RECOMP_DIRECT_OK RECOMP_DIRECT_BACK recomp_direct_target
    x87_get x87_set x87_push x87_load x87_binary x87_store x87_unary x87_to_integer x87_set_cw x87_emms
    fdrop fxch fstsw fxam ftag_put x87f_put x87f_enter x87f_spill
    mmx_get mmx_set mmx_enter simd_align xmm_f64 xmm_set_f64
""".split())
# Write flags (FCOMI, COMISS) and nothing else of the context.
FLAGS_OUT: Final = frozenset("x87_compare simd_float recomp_comis".split())

_REGISTER: Final = re.compile(r"c->r\[([0-7])\]")
_FLAG: Final = re.compile(r"c->eflags_(cf|zf|sf|of|pf|af|df)\b")
_CALL: Final = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)\(c\s*[,)]")
_RETURN: Final = re.compile(r"\breturn\s*;")
_LOCAL: Final = re.compile(r"\b(r[0-7]_|f(?:cf|zf|sf|of|pf|af|df)_)")
_WRITE: Final = re.compile(r"\b(r[0-7]_|f(?:cf|zf|sf|of|pf|af|df)_)\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|"
                           r"(?:\+\+|--)\s*(r[0-7]_|f(?:cf|zf|sf|of|pf|af|df)_)\b|"
                           r"\b(r[0-7]_|f(?:cf|zf|sf|of|pf|af|df)_)\s*(?:\+\+|--)")
_SELF_ASSIGN: Final = re.compile(r"(^\s*|[;{}]\s*)(r[0-7]_) = \2;")
SPILL: Final = "RECOMP_SPILL_"
RELOAD: Final = "RECOMP_RELOAD_"
RELOAD_FLAGS: Final = "RECOMP_RELOADF_"
# The same inside an expression (a helper whose result is used): comma-separated assignments.
SPILL_EXPR: Final = "RECOMP_SPILLE_"
RELOAD_FLAGS_EXPR: Final = "RECOMP_RELOADFE_"
# The result type of a helper called inside an expression: the temporary that carries its value past the reload
# is declared with it, so the expression keeps the promotions it had with the call in place (ISO C has no
# statement expressions). Width suffix 8/16 is uint8_t/uint16_t, everything else in use returns uint32_t.
# Helpers whose result the region code uses inside an expression. None of them writes a guest register (they read
# and write flags, x86_get_eflags reads them, recomp_in reads a port), so only the flags are loaded again after
# the call: reloading a register inside the expression that assigns it would be an unsequenced modification.
EXPRESSION_HELPERS: Final = frozenset("""
    simd_float imul2_32_f imul2_16_f shld32_f shrd32_f x86_get_eflags recomp_in
    shl8_f shl16_f shl32_f shr8_f shr16_f shr32_f sar8_f sar16_f sar32_f
    rol8_f rol16_f rol32_f ror8_f ror16_f ror32_f rcl8_f rcl16_f rcl32_f rcr8_f rcr16_f rcr32_f
""".split())
_NARROW: Final = re.compile(r"(8|16)_f$")


def _result_type(name: str) -> str:
    m = _NARROW.search(name)
    return f"uint{m.group(1)}_t" if m else "uint32_t"


def _close(text: str, open_at: int) -> int:
    """Index just past the parenthesis that closes the one at open_at."""
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1

    raise ValueError(f"unbalanced call in {text!r}")


def _statement_position(text: str, start: int, end: int) -> bool:
    before = text[:start].rstrip()
    if not text[end:].lstrip().startswith(";"):
        return False

    if not before or before[-1] in "{;}" or re.search(r"\belse$", before):
        return True

    if before[-1] == ")":  # the body of an if/while without braces, not a cast
        depth = 0
        for i in range(len(before) - 1, -1, -1):
            depth += {")": 1, "(": -1}.get(before[i], 0)
            if depth == 0:
                return re.search(r"\b(if|while|for)\s*$", before[:i]) is not None

    return False


def _line(text: str, temp: list[str]) -> str:
    code, comment = (text.split("/*", 1) + [None])[:2] if "/*" in text else (text, None)
    code = _REGISTER.sub(lambda m: f"r{m.group(1)}_", code)
    code = _FLAG.sub(lambda m: f"f{m.group(1)}_", code)
    # MOV EDI,EDI (a hot-patch point) or LEA ECX,[ECX]: in locals a self-assignment, which is a warning.
    code = _SELF_ASSIGN.sub(r"\1;", code)
    for start, name in reversed([(m.start(), m.group(1)) for m in _CALL.finditer(code)]):
        if name in PURE:
            continue

        end = _close(code, start + len(name))
        call = code[start:end]
        after = RELOAD_FLAGS if name in FLAGS_OUT else RELOAD
        before = "" if name in FLAGS_OUT else SPILL + " "
        if _statement_position(code, start, end):
            semi = code.index(";", end)
            code = code[:start] + f"{{ {before}{call}; {after} }}" + code[semi + 1:]
        else:
            if name not in EXPRESSION_HELPERS:
                raise ValueError(f"{name}: a helper called inside an expression must be in EXPRESSION_HELPERS")

            temp.append(_result_type(name))
            t = f"t{len(temp)}_"
            before_e = "" if name in FLAGS_OUT else SPILL_EXPR
            code = code[:start] + f"({before_e}{t} = {call}, {RELOAD_FLAGS_EXPR}{t})" + code[end:]

    code = _RETURN.sub(f"{{ {SPILL} return; }}", code)
    return code if comment is None else code + "/*" + comment


def body(lines: list[str]) -> tuple[list[str], str]:
    """The region's statements with the context in locals, and the declarations that open the function."""
    temp: list[str] = []
    out = [_line(text, temp) for text in lines]
    joined = "\n".join(line.split("/*", 1)[0] for line in out)
    present = set(_LOCAL.findall(joined))
    used = [n for n in NAMES if n in present]
    written_set = {m.group(1) or m.group(2) or m.group(3) for m in _WRITE.finditer(joined)}
    written = [n for n in NAMES if n in written_set]
    spill = "".join(f"{FIELD[n]} = {n}; " for n in written)
    reload = "".join(f"{n} = {FIELD[n]}; " for n in used)
    reload_flags = "".join(f"{n} = {FIELD[n]}; " for n in used if n.startswith("f"))

    def comma(statements: str) -> str:
        return statements.replace("; ", ", ")

    out = [line.replace(SPILL_EXPR, comma(spill)).replace(RELOAD_FLAGS_EXPR, comma(reload_flags))
           .replace(SPILL, spill).replace(RELOAD_FLAGS, reload_flags)
           .replace(RELOAD, reload) for line in out]
    temps = "".join(f"{t} t{i + 1}_; " for i, t in enumerate(temp))
    return out, "".join(f"uint32_t {n} = {FIELD[n]}; " for n in used) + temps
