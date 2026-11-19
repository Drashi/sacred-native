"""A small 32-bit x86 decoder: the instructions the translated programs contain, reported the way capstone 5 reports
them (mnemonic, operands with their sizes, immediates' signs), so Translator.to_insn reads it unchanged.

It replaces the capstone package on a player's machine. It covers the general-purpose, x87, MMX, 3DNow!, SSE and
SSE2 forms that occur in Sacred, Granny and TinCat, and a little around them; anything else raises DecodeError, which
the generator turns into an unqualified instruction (a trap at run time), as it does for what capstone cannot decode.
tests/lift/test_x86dec.py compares it with capstone on every instruction of the three files.
"""
from __future__ import annotations

import struct

# Operand types, with capstone's numbers (capstone.x86_const.X86_OP_*).
OP_REG = 1
OP_IMM = 2
OP_MEM = 3


class DecodeError(ValueError):
    pass


class Mem:
    __slots__ = ("segment", "base", "index", "scale", "disp")

    def __init__(self, segment=None, base=None, index=None, scale=1, disp=0):
        self.segment, self.base, self.index, self.scale, self.disp = segment, base, index, scale, disp


class Op:
    __slots__ = ("type", "reg", "imm", "mem", "size")

    def __init__(self, type_, size, reg=None, imm=0, mem=None):
        self.type, self.size, self.reg, self.imm, self.mem = type_, size, reg, imm, mem


class Insn:
    """The parts of capstone's CsInsn the translator reads. Registers are their lower-case names."""
    __slots__ = ("address", "size", "mnemonic", "operands", "bytes")

    def __init__(self, address, size, mnemonic, operands, raw):
        self.address, self.size, self.mnemonic, self.operands, self.bytes = address, size, mnemonic, operands, raw

    @staticmethod
    def reg_name(reg):
        return reg

    @property
    def op_str(self) -> str:
        parts = []
        for op in self.operands:
            if op.type == OP_REG:
                parts.append(op.reg)
            elif op.type == OP_IMM:
                parts.append("-0x%x" % -op.imm if op.imm < 0 else "0x%x" % op.imm if op.imm > 9 else str(op.imm))
            else:
                m = op.mem
                inner = " + ".join(filter(None, (m.base, f"{m.index}*{m.scale}" if m.index else None)))
                if m.disp or not inner:
                    inner += (" - " if m.disp < 0 else " + ") + "0x%x" % abs(m.disp) if inner else "0x%x" % m.disp
                parts.append(("%s:" % m.segment if m.segment else "") + "[%s]" % inner)
        return ", ".join(parts)


R32 = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")
R16 = ("ax", "cx", "dx", "bx", "sp", "bp", "si", "di")
R8 = ("al", "cl", "dl", "bl", "ah", "ch", "dh", "bh")
SEG = ("es", "cs", "ss", "ds", "fs", "gs")
MM = tuple(f"mm{i}" for i in range(8))
XMM = tuple(f"xmm{i}" for i in range(8))
ST = tuple(f"st({i})" for i in range(8))
SEGMENT_PREFIX = {0x26: "es", 0x2E: "cs", 0x36: "ss", 0x3E: "ds", 0x64: "fs", 0x65: "gs"}
CC = ("o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g")
ALU = ("add", "or", "adc", "sbb", "and", "sub", "xor", "cmp")
SHIFT = ("rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar")
# Immediates that capstone shows unsigned although the encoding sign-extends a byte.
UNSIGNED_SIGN_EXTENDED = frozenset(("and", "or", "xor"))

# x87: memory forms by (opcode, reg) -> (mnemonic, size); register forms by opcode -> per reg.
X87_MEM = {
    0xD8: [(m, 4) for m in ("fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr")],
    0xD9: [("fld", 4), None, ("fst", 4), ("fstp", 4), ("fldenv", 28), ("fldcw", 2), ("fnstenv", 28), ("fnstcw", 2)],
    0xDA: [(m, 4) for m in ("fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr")],
    0xDB: [("fild", 4), ("fisttp", 4), ("fist", 4), ("fistp", 4), None, ("fld", 10), None, ("fstp", 10)],
    0xDC: [(m, 8) for m in ("fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr")],
    0xDD: [("fld", 8), ("fisttp", 8), ("fst", 8), ("fstp", 8), ("frstor", 4), None, ("fnsave", 4), ("fnstsw", 2)],
    0xDE: [(m, 2) for m in ("fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr")],
    0xDF: [("fild", 2), ("fisttp", 2), ("fist", 2), ("fistp", 2), ("fbld", 10), ("fild", 8), ("fbstp", 10),
           ("fistp", 8)],
}
D9_SPECIAL = {0xD0: "fnop", 0xE0: "fchs", 0xE1: "fabs", 0xE4: "ftst", 0xE5: "fxam", 0xE8: "fld1", 0xE9: "fldl2t",
              0xEA: "fldl2e", 0xEB: "fldpi", 0xEC: "fldlg2", 0xED: "fldln2", 0xEE: "fldz", 0xF0: "f2xm1",
              0xF1: "fyl2x", 0xF2: "fptan", 0xF3: "fpatan", 0xF4: "fxtract", 0xF5: "fprem1", 0xF6: "fdecstp",
              0xF7: "fincstp", 0xF8: "fprem", 0xF9: "fyl2xp1", 0xFA: "fsqrt", 0xFB: "fsincos", 0xFC: "frndint",
              0xFD: "fscale", 0xFE: "fsin", 0xFF: "fcos"}

# 3DNow! (0F 0F modrm imm8): the suffix byte names the operation.
AMD3DNOW = {0x0C: "pi2fw", 0x0D: "pi2fd", 0x1C: "pf2iw", 0x1D: "pf2id", 0x8A: "pfnacc", 0x8E: "pfpnacc",
            0x90: "pfcmpge", 0x94: "pfmin", 0x96: "pfrcp", 0x97: "pfrsqrt", 0x9A: "pfsub", 0x9E: "pfadd",
            0xA0: "pfcmpgt", 0xA4: "pfmax", 0xA6: "pfrcpit1", 0xA7: "pfrsqit1", 0xAA: "pfsubr", 0xAE: "pfacc",
            0xB0: "pfcmpeq", 0xB4: "pfmul", 0xB6: "pfrcpit2", 0xB7: "pmulhrw", 0xBB: "pswapd", 0xBF: "pavgusb"}

# SSE/SSE2/MMX two-byte opcodes by (mandatory prefix, opcode): (mnemonic, operand kinds).
# Kinds: V/U xmm reg (reg/rm field), Wn xmm or memory of n bytes, P/N mm reg (reg/rm), Qn mm or memory, En r32 or
# memory, G r32 (reg field), Mn memory only, I imm8.
_W16 = ("V", "W16")
_Q8 = ("P", "Q8")
SSE = {
    (None, 0x10): ("movups", _W16), (None, 0x11): ("movups", ("W16", "V")),
    (None, 0x13): ("movlps", ("M8", "V")), (None, 0x14): ("unpcklps", _W16), (None, 0x15): ("unpckhps", _W16),
    (None, 0x17): ("movhps", ("M8", "V")), (None, 0x28): ("movaps", _W16), (None, 0x29): ("movaps", ("W16", "V")),
    (None, 0x2A): ("cvtpi2ps", ("V", "Q8")), (None, 0x2E): ("ucomiss", ("V", "W4")),
    (None, 0x2F): ("comiss", ("V", "W4")), (None, 0x51): ("sqrtps", _W16), (None, 0x52): ("rsqrtps", _W16),
    (None, 0x53): ("rcpps", _W16), (None, 0x54): ("andps", _W16), (None, 0x55): ("andnps", _W16),
    (None, 0x56): ("orps", _W16), (None, 0x57): ("xorps", _W16), (None, 0x58): ("addps", _W16),
    (None, 0x59): ("mulps", _W16), (None, 0x5B): ("cvtdq2ps", _W16), (None, 0x5C): ("subps", _W16),
    (None, 0x5D): ("minps", _W16), (None, 0x5E): ("divps", _W16), (None, 0x5F): ("maxps", _W16),
    (None, 0x60): ("punpcklbw", ("P", "Q4")), (None, 0x61): ("punpcklwd", ("P", "Q4")),
    (None, 0x62): ("punpckldq", ("P", "Q4")), (None, 0x63): ("packsswb", _Q8), (None, 0x64): ("pcmpgtb", _Q8),
    (None, 0x65): ("pcmpgtw", _Q8), (None, 0x66): ("pcmpgtd", _Q8), (None, 0x67): ("packuswb", _Q8),
    (None, 0x68): ("punpckhbw", _Q8), (None, 0x69): ("punpckhwd", _Q8), (None, 0x6A): ("punpckhdq", _Q8),
    (None, 0x6B): ("packssdw", _Q8), (None, 0x6E): ("movd", ("P", "E4")), (None, 0x6F): ("movq", _Q8),
    (None, 0x74): ("pcmpeqb", _Q8), (None, 0x75): ("pcmpeqw", _Q8), (None, 0x76): ("pcmpeqd", _Q8),
    (None, 0x7E): ("movd", ("E4", "P")), (None, 0x7F): ("movq", ("Q8", "P")), (None, 0xC6): ("shufps", _W16 + ("I",)),
    (None, 0xD1): ("psrlw", _Q8), (None, 0xD2): ("psrld", _Q8), (None, 0xD3): ("psrlq", _Q8),
    (None, 0xD5): ("pmullw", _Q8), (None, 0xD7): ("pmovmskb", ("G", "N")), (None, 0xD8): ("psubusb", _Q8),
    (None, 0xD9): ("psubusw", _Q8), (None, 0xDB): ("pand", _Q8), (None, 0xDC): ("paddusb", _Q8),
    (None, 0xDD): ("paddusw", _Q8), (None, 0xDF): ("pandn", _Q8), (None, 0xE1): ("psraw", _Q8),
    (None, 0xE2): ("psrad", _Q8), (None, 0xE5): ("pmulhw", _Q8), (None, 0xE8): ("psubsb", _Q8),
    (None, 0xE9): ("psubsw", _Q8), (None, 0xEB): ("por", _Q8), (None, 0xEC): ("paddsb", _Q8),
    (None, 0xED): ("paddsw", _Q8), (None, 0xEE): ("pmaxsw", _Q8), (None, 0xEF): ("pxor", _Q8),
    (None, 0xF1): ("psllw", _Q8), (None, 0xF2): ("pslld", _Q8), (None, 0xF3): ("psllq", _Q8),
    (None, 0xF5): ("pmaddwd", _Q8), (None, 0xF8): ("psubb", _Q8), (None, 0xF9): ("psubw", _Q8),
    (None, 0xFA): ("psubd", _Q8), (None, 0xFC): ("paddb", _Q8), (None, 0xFD): ("paddw", _Q8),
    (None, 0xFE): ("paddd", _Q8),
    (0x66, 0x10): ("movupd", _W16), (0x66, 0x11): ("movupd", ("W16", "V")), (0x66, 0x12): ("movlpd", ("V", "M8")),
    (0x66, 0x13): ("movlpd", ("M8", "V")), (0x66, 0x14): ("unpcklpd", _W16), (0x66, 0x15): ("unpckhpd", _W16),
    (0x66, 0x16): ("movhpd", ("V", "M8")), (0x66, 0x17): ("movhpd", ("M8", "V")), (0x66, 0x28): ("movapd", _W16),
    (0x66, 0x29): ("movapd", ("W16", "V")), (0x66, 0x2E): ("ucomisd", ("V", "W8")),
    (0x66, 0x2F): ("comisd", ("V", "W8")), (0x66, 0x54): ("andpd", _W16), (0x66, 0x55): ("andnpd", _W16),
    (0x66, 0x56): ("orpd", _W16), (0x66, 0x57): ("xorpd", _W16), (0x66, 0x58): ("addpd", _W16),
    (0x66, 0x59): ("mulpd", _W16), (0x66, 0x5C): ("subpd", _W16), (0x66, 0x5E): ("divpd", _W16),
    (0x66, 0x60): ("punpcklbw", _W16), (0x66, 0x61): ("punpcklwd", _W16), (0x66, 0x62): ("punpckldq", _W16),
    (0x66, 0x68): ("punpckhbw", _W16), (0x66, 0x69): ("punpckhwd", _W16), (0x66, 0x6A): ("punpckhdq", _W16),
    (0x66, 0x6C): ("punpcklqdq", _W16), (0x66, 0x6D): ("punpckhqdq", _W16), (0x66, 0x6E): ("movd", ("V", "E4")),
    (0x66, 0x6F): ("movdqa", _W16), (0x66, 0x70): ("pshufd", _W16 + ("I",)), (0x66, 0x74): ("pcmpeqb", _W16),
    (0x66, 0x75): ("pcmpeqw", _W16), (0x66, 0x76): ("pcmpeqd", _W16), (0x66, 0x7E): ("movd", ("E4", "V")),
    (0x66, 0x7F): ("movdqa", ("W16", "V")), (0x66, 0xC4): ("pinsrw", ("V", "E2", "I")),
    (0x66, 0xC5): ("pextrw", ("G", "U", "I")), (0x66, 0xC6): ("shufpd", _W16 + ("I",)),
    (0x66, 0xD3): ("psrlq", _W16), (0x66, 0xD4): ("paddq", _W16), (0x66, 0xD6): ("movq", ("W8", "V")),
    (0x66, 0xD7): ("pmovmskb", ("G", "U")), (0x66, 0xDB): ("pand", _W16), (0x66, 0xDF): ("pandn", _W16),
    (0x66, 0xEB): ("por", _W16), (0x66, 0xEE): ("pmaxsw", _W16), (0x66, 0xEF): ("pxor", _W16),
    (0x66, 0xF3): ("psllq", _W16), (0x66, 0xFA): ("psubd", _W16), (0x66, 0xFB): ("psubq", _W16),
    (0x66, 0xFE): ("paddd", _W16),
    (0xF2, 0x10): ("movsd", ("V", "W8")), (0xF2, 0x11): ("movsd", ("W8", "V")), (0xF2, 0x2C): ("cvttsd2si", ("G", "W8")),
    (0xF2, 0x2D): ("cvtsd2si", ("G", "W8")), (0xF2, 0x51): ("sqrtsd", ("V", "W8")), (0xF2, 0x58): ("addsd", ("V", "W8")),
    (0xF2, 0x59): ("mulsd", ("V", "W8")), (0xF2, 0x5A): ("cvtsd2ss", ("V", "W8")), (0xF2, 0x5C): ("subsd", ("V", "W8")),
    (0xF2, 0x5D): ("minsd", ("V", "W8")), (0xF2, 0x5E): ("divsd", ("V", "W8")), (0xF2, 0x5F): ("maxsd", ("V", "W8")),
    (0xF3, 0x10): ("movss", ("V", "W4")), (0xF3, 0x11): ("movss", ("W4", "V")), (0xF3, 0x2C): ("cvttss2si", ("G", "W4")),
    (0xF3, 0x2D): ("cvtss2si", ("G", "W4")), (0xF3, 0x51): ("sqrtss", ("V", "W4")), (0xF3, 0x52): ("rsqrtss", ("V", "W4")),
    (0xF3, 0x53): ("rcpss", ("V", "W4")), (0xF3, 0x58): ("addss", ("V", "W4")), (0xF3, 0x59): ("mulss", ("V", "W4")),
    (0xF3, 0x5A): ("cvtss2sd", ("V", "W4")), (0xF3, 0x5C): ("subss", ("V", "W4")), (0xF3, 0x5D): ("minss", ("V", "W4")),
    (0xF3, 0x5E): ("divss", ("V", "W4")), (0xF3, 0x5F): ("maxss", ("V", "W4")), (0xF3, 0x7E): ("movq", ("V", "W8")),
    (0xF3, 0xE6): ("cvtdq2pd", ("V", "W8")),
}
# 0F 12 / 0F 16 without a prefix: a memory operand is movlps/movhps, a register one movhlps/movlhps.
SSE_SPLIT = {0x12: (("movlps", ("V", "M8")), ("movhlps", ("V", "U"))),
             0x16: (("movhps", ("V", "M8")), ("movlhps", ("V", "U")))}
# MMX/SSE shift by immediate: 0F 71/72/73 /reg.
SHIFT_IMM = {(0x71, 2): "psrlw", (0x71, 4): "psraw", (0x71, 6): "psllw", (0x72, 2): "psrld", (0x72, 4): "psrad",
             (0x72, 6): "pslld", (0x73, 2): "psrlq", (0x73, 3): "psrldq", (0x73, 6): "psllq", (0x73, 7): "pslldq"}
CMP_PREDICATE = ("eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord")


class _Reader:
    __slots__ = ("code", "pos")

    def __init__(self, code: bytes, pos: int):
        self.code, self.pos = code, pos

    def u8(self) -> int:
        if self.pos >= len(self.code):
            raise DecodeError("truncated")
        self.pos += 1
        return self.code[self.pos - 1]

    def s8(self) -> int:
        value = self.u8()
        return value - 0x100 if value & 0x80 else value

    def u16(self) -> int:
        if self.pos + 2 > len(self.code):
            raise DecodeError("truncated")
        self.pos += 2
        return struct.unpack_from("<H", self.code, self.pos - 2)[0]

    def u32(self) -> int:
        if self.pos + 4 > len(self.code):
            raise DecodeError("truncated")
        self.pos += 4
        return struct.unpack_from("<I", self.code, self.pos - 4)[0]

    def s32(self) -> int:
        value = self.u32()
        return value - 0x100000000 if value & 0x80000000 else value


def _modrm(reader: _Reader, segment):
    """(mod, reg, rm, Mem or None)."""
    modrm = reader.u8()
    mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
    if mod == 3:
        return mod, reg, rm, None
    base = index = None
    scale = 1
    if rm == 4:
        sib = reader.u8()
        scale, idx, b = 1 << (sib >> 6), (sib >> 3) & 7, sib & 7
        if idx != 4:
            index = R32[idx]
        else:
            scale = 1
        if b == 5 and mod == 0:
            disp = reader.s32()
        else:
            base = R32[b]
            disp = 0
    elif rm == 5 and mod == 0:
        disp = reader.s32()
    else:
        base = R32[rm]
        disp = 0
    if mod == 1:
        disp = reader.s8()
    elif mod == 2:
        disp = reader.s32()
    return mod, reg, rm, Mem(segment, base, index, scale, disp)


def _reg(name, size):
    return Op(OP_REG, size, reg=name)


def _imm(value, size):
    return Op(OP_IMM, size, imm=value)


def _mem(mem, size):
    return Op(OP_MEM, size, mem=mem)


def _gpr(index, size):
    return _reg((R8 if size == 1 else R16 if size == 2 else R32)[index], size)


def decode(code: bytes, address: int, offset: int = 0) -> Insn:
    """Decode the instruction at code[offset:] that lies at `address`."""
    reader = _Reader(code, offset)
    segment = None
    opsize = 4
    rep = None          # 0xF2 / 0xF3, the last one wins
    lock = False
    mandatory = None    # the SSE prefix: the last of 66/F2/F3
    while True:
        if reader.pos - offset >= 15:
            raise DecodeError("too many prefixes")
        byte = reader.u8()
        if byte in SEGMENT_PREFIX:
            segment = SEGMENT_PREFIX[byte]
        elif byte == 0x66:
            opsize = 2
            mandatory = mandatory or 0x66
        elif byte in (0xF2, 0xF3):
            rep = byte
            mandatory = byte
        elif byte == 0xF0:
            lock = True
        elif byte == 0x67:
            pass
        else:
            break
    ops: list[Op] = []
    mnemonic = None

    def finish(name, operands):
        prefix = "lock " if lock else ""
        size = reader.pos - offset
        return Insn(address, size, prefix + name, operands, bytes(code[offset:offset + size]))

    def ev(mem, rm, size):
        return _mem(mem, size) if mem is not None else _gpr(rm, size)

    def signed_byte(name, size):
        value = reader.s8()
        if name in UNSIGNED_SIGN_EXTENDED and value < 0:
            value &= (1 << (8 * size)) - 1
        return _imm(value, size)

    def imm_z(size):
        return _imm(reader.u16() if size == 2 else reader.u32(), size)

    op = byte
    if op == 0x9B:
        return finish("wait", [])
    if op == 0x0F:
        return _two_byte(reader, offset, address, code, segment, opsize, rep, mandatory, finish, ev)
    if op < 0x40 and op & 7 < 6:
        name, form = ALU[op >> 3], op & 7
        if form < 4:
            size = 1 if form % 2 == 0 else opsize
            _, reg, rm, mem = _modrm(reader, segment)
            e, g = ev(mem, rm, size), _gpr(reg, size)
            return finish(name, [e, g] if form < 2 else [g, e])
        size = 1 if form == 4 else opsize
        return finish(name, [_gpr(0, size), _imm(reader.u8(), 1) if size == 1 else imm_z(size)])
    if op in (0x06, 0x0E, 0x16, 0x1E):
        return finish("push", [_reg(SEG[op >> 3], 2)])
    if op in (0x07, 0x17, 0x1F):
        return finish("pop", [_reg(SEG[op >> 3], 2)])
    if 0x40 <= op < 0x50:
        return finish("inc" if op < 0x48 else "dec", [_gpr(op & 7, opsize)])
    if 0x50 <= op < 0x60:
        return finish("push" if op < 0x58 else "pop", [_gpr(op & 7, opsize)])
    if op == 0x60:
        return finish("pushal" if opsize == 4 else "pushaw", [])
    if op == 0x61:
        return finish("popal" if opsize == 4 else "popaw", [])
    if op == 0x68:
        return finish("push", [imm_z(opsize)])
    if op == 0x6A:
        return finish("push", [signed_byte("push", opsize)])
    if op in (0x69, 0x6B):
        _, reg, rm, mem = _modrm(reader, segment)
        operands = [_gpr(reg, opsize), ev(mem, rm, opsize)]
        return finish("imul", operands + [imm_z(opsize) if op == 0x69 else signed_byte("imul", opsize)])
    if 0x70 <= op < 0x80:
        rel = reader.s8()
        return finish("j" + CC[op & 15], [_imm((address + reader.pos - offset + rel) & 0xFFFFFFFF, 4)])
    if op in (0x80, 0x81, 0x83):
        size = 1 if op == 0x80 else opsize
        _, reg, rm, mem = _modrm(reader, segment)
        name = ALU[reg]
        if op == 0x83:
            value = signed_byte(name, size)
        else:
            value = _imm(reader.u8(), 1) if size == 1 else imm_z(size)
        return finish(name, [ev(mem, rm, size), value])
    if op in (0x84, 0x85, 0x86, 0x87):
        size = 1 if op % 2 == 0 else opsize
        _, reg, rm, mem = _modrm(reader, segment)
        return finish("test" if op < 0x86 else "xchg", [ev(mem, rm, size), _gpr(reg, size)])
    if 0x88 <= op <= 0x8B:
        size = 1 if op % 2 == 0 else opsize
        _, reg, rm, mem = _modrm(reader, segment)
        e, g = ev(mem, rm, size), _gpr(reg, size)
        return finish("mov", [e, g] if op < 0x8A else [g, e])
    if op == 0x8D:
        _, reg, rm, mem = _modrm(reader, segment)
        if mem is None:
            raise DecodeError("lea with a register operand")
        return finish("lea", [_gpr(reg, opsize), _mem(mem, opsize)])
    if op == 0x8F:
        _, reg, rm, mem = _modrm(reader, segment)
        if reg:
            raise DecodeError("8f /%d" % reg)
        return finish("pop", [ev(mem, rm, opsize)])
    if op == 0x90 and rep is None:
        return finish("nop", [])
    if 0x91 <= op <= 0x97:
        return finish("xchg", [_gpr(op & 7, opsize), _gpr(0, opsize)])
    if op == 0x98:
        return finish("cwde" if opsize == 4 else "cbw", [])
    if op == 0x99:
        return finish("cdq" if opsize == 4 else "cwd", [])
    simple = {0x9C: "pushfd" if opsize == 4 else "pushf", 0x9D: "popfd" if opsize == 4 else "popf", 0x9E: "sahf",
              0x9F: "lahf", 0xC3: "ret", 0xC9: "leave", 0xCC: "int3", 0xD7: "xlatb", 0xF4: "hlt", 0xF5: "cmc",
              0xF8: "clc", 0xF9: "stc", 0xFA: "cli", 0xFB: "sti", 0xFC: "cld", 0xFD: "std"}
    if op in simple:
        return finish(simple[op], [])
    if 0xA0 <= op <= 0xA3:
        size = 1 if op % 2 == 0 else opsize
        mem = _mem(Mem(segment, None, None, 1, reader.s32()), size)
        acc = _gpr(0, size)
        return finish("mov", [acc, mem] if op < 0xA2 else [mem, acc])
    if 0xA4 <= op <= 0xAF and op not in (0xA8, 0xA9):
        size = 1 if op % 2 == 0 else opsize
        suffix = "b" if size == 1 else "w" if size == 2 else "d"
        dst = _mem(Mem("es", "edi"), size)
        src = _mem(Mem(segment, "esi"), size)
        acc = _gpr(0, size)
        kind = (op - 0xA4) >> 1
        name, operands = (("movs", [dst, src]), ("cmps", [src, dst]), None, ("stos", [dst, acc]),
                          ("lods", [acc, src]), ("scas", [acc, dst]))[kind]
        if rep == 0xF3:
            name = ("repe " if kind in (1, 5) else "rep ") + name
        elif rep == 0xF2:
            name = "repne " + name
        return finish(name + suffix, operands)
    if op in (0xA8, 0xA9):
        size = 1 if op == 0xA8 else opsize
        return finish("test", [_gpr(0, size), _imm(reader.u8(), 1) if size == 1 else imm_z(size)])
    if 0xB0 <= op < 0xB8:
        return finish("mov", [_gpr(op & 7, 1), _imm(reader.u8(), 1)])
    if 0xB8 <= op < 0xC0:
        return finish("mov", [_gpr(op & 7, opsize), imm_z(opsize)])
    if op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3):
        size = 1 if op % 2 == 0 else opsize
        _, reg, rm, mem = _modrm(reader, segment)
        if op in (0xC0, 0xC1):
            count = _imm(reader.u8(), 1)
        elif op in (0xD0, 0xD1):
            count = _imm(1, size)
        else:
            count = _reg("cl", 1)
        return finish(SHIFT[reg], [ev(mem, rm, size), count])
    if op == 0xC2:
        return finish("ret", [_imm(reader.u16(), 2)])
    if op in (0xC4, 0xC5):
        _, reg, rm, mem = _modrm(reader, segment)
        if mem is None:
            raise DecodeError("les/lds with a register operand")
        return finish("les" if op == 0xC4 else "lds", [_gpr(reg, opsize), _mem(mem, opsize)])
    if op in (0xC6, 0xC7):
        size = 1 if op == 0xC6 else opsize
        _, reg, rm, mem = _modrm(reader, segment)
        if reg:
            raise DecodeError("c6/c7 /%d" % reg)
        return finish("mov", [ev(mem, rm, size), _imm(reader.u8(), 1) if size == 1 else imm_z(size)])
    if 0xD8 <= op <= 0xDF:
        return _x87(reader, op, segment, finish)
    if op == 0xE3:
        rel = reader.s8()
        return finish("jecxz", [_imm((address + reader.pos - offset + rel) & 0xFFFFFFFF, 4)])
    if op in (0xE8, 0xE9):
        rel = reader.s32()
        return finish("call" if op == 0xE8 else "jmp", [_imm((address + reader.pos - offset + rel) & 0xFFFFFFFF, 4)])
    if op == 0xEB:
        rel = reader.s8()
        return finish("jmp", [_imm((address + reader.pos - offset + rel) & 0xFFFFFFFF, 4)])
    if op in (0xEC, 0xED):
        size = 1 if op == 0xEC else opsize
        return finish("in", [_gpr(0, size), _reg("dx", 2)])
    if op in (0xF6, 0xF7):
        size = 1 if op == 0xF6 else opsize
        _, reg, rm, mem = _modrm(reader, segment)
        name = ("test", "test", "not", "neg", "mul", "imul", "div", "idiv")[reg]
        operands = [ev(mem, rm, size)]
        if reg < 2:
            operands.append(_imm(reader.u8(), 1) if size == 1 else imm_z(size))
        return finish(name, operands)
    if op == 0xFE:
        _, reg, rm, mem = _modrm(reader, segment)
        if reg > 1:
            raise DecodeError("fe /%d" % reg)
        return finish(("inc", "dec")[reg], [ev(mem, rm, 1)])
    if op == 0xFF:
        _, reg, rm, mem = _modrm(reader, segment)
        name = ("inc", "dec", "call", None, "jmp", None, "push", None)[reg]
        if name is None:
            raise DecodeError("ff /%d" % reg)
        return finish(name, [ev(mem, rm, opsize if reg in (0, 1, 6) else 4)])
    raise DecodeError("opcode %02x" % op)


def _x87(reader, op, segment, finish):
    mod, reg, rm, mem = _modrm(reader, segment)
    if mem is not None:
        entry = X87_MEM[op][reg]
        if entry is None:
            raise DecodeError("x87 %02x /%d memory" % (op, reg))
        return finish(entry[0], [_mem(mem, entry[1])])
    st = _reg(ST[rm], 10)
    st0 = _reg(ST[0], 10)
    modrm = 0xC0 | reg << 3 | rm
    if op == 0xD8:
        return finish(("fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr")[reg], [st])
    if op == 0xD9:
        if reg == 0:
            return finish("fld", [st])
        if reg == 1:
            return finish("fxch", [st0, st])
        if modrm in D9_SPECIAL:
            return finish(D9_SPECIAL[modrm], [])
    if op == 0xDA and modrm == 0xE9:
        return finish("fucompp", [])
    if op == 0xDB and modrm == 0xE2:
        return finish("fnclex", [])
    if op == 0xDB and modrm == 0xE3:
        return finish("fninit", [])
    if op == 0xDC and reg not in (2, 3):
        return finish(("fadd", "fmul", None, None, "fsubr", "fsub", "fdivr", "fdiv")[reg], [st, st0])
    if op == 0xDD:
        name = ("ffree", None, "fst", "fstp", "fucom", "fucomp", None, None)[reg]
        if name:
            return finish(name, [st])
    if op == 0xDE:
        if modrm == 0xD9:
            return finish("fcompp", [])
        name = ("faddp", "fmulp", None, None, "fsubrp", "fsubp", "fdivrp", "fdivp")[reg]
        if name:
            return finish(name, [st])
    if op == 0xDF and modrm == 0xE0:
        return finish("fnstsw", [_reg("ax", 2)])
    raise DecodeError("x87 %02x %02x" % (op, modrm))


def _two_byte(reader, offset, address, code, segment, opsize, rep, mandatory, finish, ev):
    op = reader.u8()
    if 0x80 <= op < 0x90:
        rel = reader.s32()
        return finish("j" + CC[op & 15], [_imm((address + reader.pos - offset + rel) & 0xFFFFFFFF, 4)])
    if 0x90 <= op < 0xA0:
        _, reg, rm, mem = _modrm(reader, segment)
        return finish("set" + CC[op & 15], [ev(mem, rm, 1)])
    simple = {0x0E: "femms", 0x31: "rdtsc", 0x77: "emms", 0xA2: "cpuid"}
    if op in simple:
        return finish(simple[op], [])
    if op in (0xB6, 0xB7, 0xBE, 0xBF):
        _, reg, rm, mem = _modrm(reader, segment)
        source = 1 if op in (0xB6, 0xBE) else 2
        return finish("movzx" if op < 0xB8 else "movsx", [_gpr(reg, opsize), ev(mem, rm, source)])
    if op == 0xAF:
        _, reg, rm, mem = _modrm(reader, segment)
        return finish("imul", [_gpr(reg, opsize), ev(mem, rm, opsize)])
    if op in (0xA3, 0xAB, 0xB3, 0xBB):
        _, reg, rm, mem = _modrm(reader, segment)
        return finish({0xA3: "bt", 0xAB: "bts", 0xB3: "btr", 0xBB: "btc"}[op], [ev(mem, rm, opsize), _gpr(reg, opsize)])
    if op in (0xA4, 0xA5, 0xAC, 0xAD):
        _, reg, rm, mem = _modrm(reader, segment)
        count = _imm(reader.u8(), 1) if op in (0xA4, 0xAC) else _reg("cl", 1)
        return finish("shld" if op < 0xA8 else "shrd", [ev(mem, rm, opsize), _gpr(reg, opsize), count])
    if op == 0xAE:
        _, reg, rm, mem = _modrm(reader, segment)
        if mem is not None and reg in (2, 3):
            return finish(("ldmxcsr", "stmxcsr")[reg - 2], [_mem(mem, 4)])
        raise DecodeError("0f ae /%d" % reg)
    if op == 0x0F:  # 3DNow!: the operation follows the operands
        _, reg, rm, mem = _modrm(reader, segment)
        name = AMD3DNOW.get(reader.u8())
        if name is None:
            raise DecodeError("3DNow! %02x" % code[reader.pos - 1])
        return finish(name, [_reg(MM[reg], 8), _mem(mem, 8) if mem is not None else _reg(MM[rm], 8)])
    xmm = mandatory == 0x66
    if op in (0x71, 0x72, 0x73):
        _, reg, rm, mem = _modrm(reader, segment)
        name = SHIFT_IMM.get((op, reg))
        if mem is not None or name is None or (reg in (3, 7) and not xmm):
            raise DecodeError("0f %02x /%d" % (op, reg))
        return finish(name, [_reg(XMM[rm], 16) if xmm else _reg(MM[rm], 8), _imm(reader.u8(), 1)])
    if op == 0xC2:
        _, reg, rm, mem = _modrm(reader, segment)
        width = {None: ("ps", 16), 0x66: ("pd", 16), 0xF3: ("ss", 4), 0xF2: ("sd", 8)}[mandatory]
        predicate = reader.u8()
        if predicate > 7:
            raise DecodeError("cmpps predicate %d" % predicate)
        source = _mem(mem, width[1]) if mem is not None else _reg(XMM[rm], 16)
        return finish("cmp" + CMP_PREDICATE[predicate] + width[0], [_reg(XMM[reg], 16), source])
    if mandatory is None and op in SSE_SPLIT:
        pos = reader.pos
        is_mem = code[pos] >> 6 != 3 if pos < len(code) else False
        name, kinds = SSE_SPLIT[op][0 if is_mem else 1]
    else:
        entry = SSE.get((mandatory, op))
        if entry is None:
            raise DecodeError("0f %02x with prefix %s" % (op, mandatory))
        name, kinds = entry
    _, reg, rm, mem = _modrm(reader, segment)
    operands = []
    for kind in kinds:
        letter, width = kind[0], int(kind[1:]) if len(kind) > 1 else 0
        if letter == "V":
            operands.append(_reg(XMM[reg], 16))
        elif letter == "U":
            if mem is not None:
                raise DecodeError("%s needs a register" % name)
            operands.append(_reg(XMM[rm], 16))
        elif letter == "P":
            operands.append(_reg(MM[reg], 8))
        elif letter == "N":
            if mem is not None:
                raise DecodeError("%s needs a register" % name)
            operands.append(_reg(MM[rm], 8))
        elif letter == "G":
            operands.append(_reg(R32[reg], 4))
        elif letter == "W":
            operands.append(_mem(mem, width) if mem is not None else _reg(XMM[rm], 16))
        elif letter == "Q":
            operands.append(_mem(mem, width) if mem is not None else _reg(MM[rm], 8))
        elif letter == "E":
            operands.append(_mem(mem, width) if mem is not None else _reg(R32[rm], 4))
        elif letter == "M":
            if mem is None:
                raise DecodeError("%s needs memory" % name)
            operands.append(_mem(mem, width))
        elif letter == "I":
            operands.append(_imm(reader.u8(), 1))
    return finish(name, operands)


class Decoder:
    """capstone.Cs(CS_ARCH_X86, CS_MODE_32) as the translator uses it: disasm() yields instructions until the bytes
    run out or one does not decode."""
    detail = True

    def disasm(self, code: bytes, address: int, count: int = 0):
        offset = 0
        done = 0
        while offset < len(code) and (not count or done < count):
            try:
                insn = decode(code, address + offset, offset)
            except DecodeError:
                return
            yield insn
            offset += insn.size
            done += 1
