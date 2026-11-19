"""Portable lane lowering for the SIMD forms in the Sacred export."""
import re
from translate import MMX_BINARY, MMX_SHIFT, TranslateError, addr_expr, parse_imm, read_op, write_op

FLOAT = {"ADDPS": "ADD", "ADDSS": "ADD", "SUBPS": "SUB", "SUBSS": "SUB",
         "MULPS": "MUL", "MULSS": "MUL", "RCPPS": "RCP", "RCPSS": "RCP",
         "RSQRTSS": "RSQRT", "CMPNEQPS": "CMP_NE", "COMISS": "COMIS",
         "PFADD": "ADD", "PFSUB": "SUB", "PFSUBR": "SUB", "PFMUL": "MUL",
         "PFCMPEQ": "CMP_EQ", "PFCMPGT": "CMP_GT", "PFCMPGE": "CMP_GE",
         "PFMIN": "MIN", "PFMAX": "MAX", "PFRCP": "RCP", "PFRSQRT": "RSQRT",
         "PI2FD": "FROM_INT", "PF2ID": "TO_INT"}


def lane(op, i):
    if op.kind == "xmm":
        return "c->xmm[%d][%d]" % (op.reg, i)
    if op.kind == "mm":
        return "(uint32_t)(mmx_get(c, %d) >> %d)" % (op.reg, 32 * i)
    return "rd32(%s + %du)" % (addr_expr(op), 4 * i)


def put(op, i, value):
    if op.kind == "xmm":
        return "c->xmm[%d][%d] = %s;" % (op.reg, i, value)
    return "wr32(%s + %du, %s);" % (addr_expr(op), 4 * i, value)


def emit(t, ins, m, ops):
    if m in ("EMMS", "FEMMS"):
        return ["x87_emms(c);"]
    if m in ("LDMXCSR", "STMXCSR"):
        return ["c->mxcsr = rd32(%s);" % addr_expr(ops[0])] if m == "LDMXCSR" else [
            "wr32(%s, c->mxcsr);" % addr_expr(ops[0])]
    if not ops:
        return None
    dst = ops[0]
    if m in ("MOVSS", "MOVSD") and any(op.kind == "xmm" for op in ops):
        n = 1 if m == "MOVSS" else 2
        lines = ["uint32_t s%d_ = %s;" % (i, lane(ops[1], i)) for i in range(n)]
        lines += [put(dst, i, "s%d_" % i) for i in range(n)]
        if dst.kind == "xmm" and ops[1].kind == "mem":
            lines += [put(dst, i, "0u") for i in range(n, 4)]
        return lines
    if m in FLOAT:
        source = ops[1]
        now = dst.kind == "mm"
        n = 2 if now else 1 if m.endswith("SS") else 4
        count = 1 if m in ("PFRCP", "PFRSQRT") else n
        lines = ["uint32_t d%d_ = %s, s%d_ = %s;" % (i, lane(dst, i), i, lane(source, i))
                 for i in range(count)]
        for i in range(count):
            a, b = ("s%d_" % i, "d%d_" % i) if m == "PFSUBR" else ("d%d_" % i, "s%d_" % i)
            if m == "PFRSQRT":
                b = "(s0_ & 0x7fffffffu)"
            if m == "COMISS":
                lines += ["(void)simd_float(c, %s, %s, SIMD_%s, %d);" % (a, b, FLOAT[m], now)]
                continue
            lines += ["uint32_t r%d_ = simd_float(c, %s, %s, SIMD_%s, %d);" %
                      (i, a, b, FLOAT[m], now)]
            if m == "PFRSQRT":
                lines += ["r0_ |= s0_ & 0x80000000u;"]
        if m == "COMISS":
            return lines
        if now:
            lines += ["mmx_set(c, %d, (uint64_t)r0_ | ((uint64_t)r%d_ << 32));" %
                      (dst.reg, 0 if count == 1 else 1)]
        else:
            lines += [put(dst, i, "r%d_" % i) for i in range(n)]
        return lines
    if m in ("PFACC", "PFNACC", "PFPNACC"):
        lines = ["uint32_t d0_ = %s, d1_ = %s, s0_ = %s, s1_ = %s;" %
                 (lane(dst, 0), lane(dst, 1), lane(ops[1], 0), lane(ops[1], 1))]
        lines += ["uint32_t r0_ = simd_float(c, d0_, d1_, SIMD_%s, 1);" %
                  ("ADD" if m == "PFACC" else "SUB"),
                  "uint32_t r1_ = simd_float(c, s0_, s1_, SIMD_%s, 1);" %
                  ("SUB" if m == "PFNACC" else "ADD"),
                  "mmx_set(c, %d, (uint64_t)r0_ | ((uint64_t)r1_ << 32));" % dst.reg]
        return lines
    if m in ("PFRCPIT1", "PFRCPIT2", "PFRSQIT1", "PSWAPD"):
        # QEMU's reciprocal seed is already fully rounded; refinements copy src.
        lines = ["uint32_t s0_ = %s, s1_ = %s;" % (lane(ops[1], 0), lane(ops[1], 1))]
        lo, hi = ("s1_", "s0_") if m == "PSWAPD" else ("s0_", "s1_")
        return lines + ["mmx_set(c, %d, (uint64_t)%s | ((uint64_t)%s << 32));" % (dst.reg, lo, hi)]
    if m in ("CVTPI2PS", "CVTDQ2PS"):
        n = 2 if m == "CVTPI2PS" else 4
        lines = ["uint32_t s%d_ = %s;" % (i, lane(ops[1], i)) for i in range(n)]
        return lines + [put(dst, i, "simd_float(c, 0, s%d_, SIMD_FROM_INT, 0)" % i) for i in range(n)]
    if m == "PMOVMSKB":
        n = 4 if ops[1].kind == "xmm" else 2
        lines = ["uint32_t mask_ = 0;"]
        for i in range(n):
            lines += ["uint32_t s%d_ = %s;" % (i, lane(ops[1], i))]
            lines += ["mask_ |= ((s%d_ >> %d) & 1u) << %d;" % (i, j * 8 + 7, i * 4 + j)
                      for j in range(4)]
        return lines + [write_op(dst, 32, "mask_")]
    if m in ("MOVHLPS", "MOVLHPS"):
        start = 2 if m == "MOVHLPS" else 0
        out = 0 if m == "MOVHLPS" else 2
        return ["uint32_t s0_ = %s, s1_ = %s;" % (lane(ops[1], start), lane(ops[1], start + 1)),
                put(dst, out, "s0_"), put(dst, out + 1, "s1_")]
    if m in ("UNPCKLPS", "UNPCKHPS", "PUNPCKLWD", "PUNPCKHWD") and dst.kind == "xmm":
        half = 2 if m in ("UNPCKHPS", "PUNPCKHWD") else 0
        if m.startswith("UNPCK"):
            values = ["d%d_" % half, "s%d_" % half, "d%d_" % (half + 1), "s%d_" % (half + 1)]
        else:
            values = ["((d%d_ >> %d) & 0xffffu) | ((s%d_ >> %d) << 16)" %
                      (half + i // 2, 16 * (i % 2), half + i // 2, 16 * (i % 2))
                      for i in range(4)]
        # Declare only the lanes the stores read; a memory operand is still read
        # in full (unused lanes are discarded) so a faulting access stays observable.
        used = " ".join(values)
        lines = []
        for i in range(4):
            for prefix, op in (("d", dst), ("s", ops[1])):
                if re.search(r"\b%s%d_\b" % (prefix, i), used):
                    lines.append("uint32_t %s%d_ = %s;" % (prefix, i, lane(op, i)))
                elif op.kind == "mem":
                    lines.append("(void)(%s);" % lane(op, i))
        return lines + [put(dst, i, value) for i, value in enumerate(values)]
    if m in ("MOVAPS", "MOVDQA"):
        lines = ["simd_align(c, %s);" % addr_expr(op) for op in ops if op.kind == "mem"]
        lines += ["uint32_t s%d_ = %s;" % (i, lane(ops[1], i)) for i in range(4)]
        return lines + [put(dst, i, "s%d_" % i) for i in range(4)]
    if m in ("MOVLPS", "MOVHPS"):
        half = 2 if m == "MOVHPS" else 0
        start = 0 if dst.kind == "xmm" else half
        out = half if dst.kind == "xmm" else 0
        return ["uint32_t s0_ = %s, s1_ = %s;" % (lane(ops[1], start), lane(ops[1], start + 1)),
                put(dst, out, "s0_"), put(dst, out + 1, "s1_")]
    if m == "PSLLD" and dst.kind == "xmm" and ops[1].kind != "imm":
        return None
    if m == "PMAXSW" or (dst.kind == "xmm" and m in ("PCMPEQW", "PADDD", "PSUBD", "PSLLD")):
        n = 4 if dst.kind == "xmm" else 2
        # An immediate count is passed straight to the helper, so it has no s temporaries.
        lines = [("uint32_t d%d_ = %s;" % (i, lane(dst, i))) if ops[1].kind == "imm" else
                 "uint32_t d%d_ = %s, s%d_ = %s;" % (i, lane(dst, i), i, lane(ops[1], i))
                 for i in range(n)]
        if m == "PMAXSW":
            for i in range(n):
                lines += ["uint32_t r%d_ = 0;" % i]
                for shift in (0, 16):
                    lines += ["r%d_ |= (uint32_t)(uint16_t)((int16_t)(d%d_ >> %d) > "
                              "(int16_t)(s%d_ >> %d) ? (d%d_ >> %d) : (s%d_ >> %d)) << %d;" %
                              (i, i, shift, i, shift, i, shift, i, shift, shift)]
            if n == 2:
                return lines + ["mmx_set(c, %d, (uint64_t)r0_ | ((uint64_t)r1_ << 32));" % dst.reg]
            return lines + [put(dst, i, "r%d_" % i) for i in range(n)]
        helper, bits = MMX_SHIFT[m] if m in MMX_SHIFT else MMX_BINARY[m]
        for half in range(2):
            i = half * 2
            a = "((uint64_t)d%d_ | ((uint64_t)d%d_ << 32))" % (i, i + 1)
            j = 0 if m in MMX_SHIFT else i
            b = "((uint64_t)s%d_ | ((uint64_t)s%d_ << 32))" % (j, j + 1)
            if ops[1].kind == "imm":
                b = str(parse_imm(ins.ops[1])) + "ull"
            call = "%s(%s, %s%s)" % (helper, a, b, ", %du" % bits if bits else "")
            lines += ["uint64_t r%d_ = %s;" % (half, call)]
        return lines + [put(dst, i, "(uint32_t)(r%d_ >> %d)" % (i // 2, (i % 2) * 32)) for i in range(4)]
    return None
