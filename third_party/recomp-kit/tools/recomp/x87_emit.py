"""Literal x87 lowering to the project's software ext80 helpers."""
from translate import TranslateError, addr_expr, hexlit


def emit(t, ins, m, ops):
    get = lambda i: "x87_get(c, %d)" % i
    put = lambda i, v: "x87_set(c, %d, %s);" % (i, v)
    load = lambda op: "x87_load(c, %s, %d)" % (addr_expr(op), op.size)
    integer = lambda op: "x87_integer((int%d_t)rd%d(%s))" % (op.size, op.size, addr_expr(op))
    lines = []
    control = {"FLDCW", "FNSTCW", "FSTCW", "FNSTSW", "FSTSW", "FNSAVE", "FSAVE",
               "FRSTOR", "FNSTENV", "FSTENV", "FLDENV", "FINIT", "FNINIT",
               "FNCLEX", "FCLEX", "FNOP"}
    if not m.startswith("FN"):
        lines.append("x87_wait(c);")
    if m not in control:
        at = ins.addr
        while t.image.rd8(at) in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0x9b):
            at += 1
        opcode = ((t.image.rd8(at) & 7) << 8) | t.image.rd8(at + 1)
        lines += ["c->fpu_ip = %s; c->fpu_opcode = 0x%x; c->fpu_cs = c->cs;" %
                  (hexlit(ins.addr), opcode)]
        memory = next((op for op in ops if op.kind == "mem"), None)
        if memory is not None:
            lines += ["c->fpu_dp = %s; c->fpu_ds = c->ds;" % addr_expr(memory)]
    body = []
    if m == "FLD":
        body = ["x87_push(c, %s);" % (get(ops[0].sti) if ops[0].kind == "st" else load(ops[0]))]
    elif m in ("FLD1", "FLDZ", "FLDPI", "FLDLN2", "FLDL2E", "FLDLG2", "FLDL2T"):
        constants = {
            "FLD1": (0x8000000000000000, 0x3fff), "FLDZ": (0, 0),
            "FLDPI": (0xc90fdaa22168c235, 0x4000), "FLDLN2": (0xb17217f7d1cf79ac, 0x3ffe),
            "FLDL2E": (0xb8aa3b295c17f0bc, 0x3fff), "FLDLG2": (0x9a209a84fbcff799, 0x3ffd),
            "FLDL2T": (0xd49a784bcd1b8afe, 0x4000)}
        bits, exponent = constants[m]
        # Constants have RC-specific last bits; PC does not affect loads.
        downward = {"FLDPI", "FLDLN2", "FLDLG2", "FLDL2E"}
        rc_adjust = "- ((c->fpu_cw & 0xc00u) == 0x400u || (c->fpu_cw & 0xc00u) == 0xc00u)" if m in downward else (
            "+ ((c->fpu_cw & 0xc00u) == 0x800u)" if m == "FLDL2T" else "")
        body = ["X87Value v_ = {UINT64_C(0x%x) %s, 0x%x};" % (bits, rc_adjust, exponent),
                "x87_push(c, v_);"]
    elif m == "FILD":
        body = ["x87_push(c, %s);" % integer(ops[0])]
    elif m == "FBSTP":
        body = ["x87_store_bcd(c, %s);" % addr_expr(ops[0]), "fdrop(c);"]
    elif m in ("FST", "FSTP"):
        body = [put(ops[0].sti, get(0))] if ops[0].kind == "st" else [
            "x87_store(c, %s, %d);" % (addr_expr(ops[0]), ops[0].size)]
        if m == "FSTP":
            body += ["fdrop(c);"]
    elif m in ("FIST", "FISTP", "FISTTP"):
        op = ops[0]
        body = ["wr%d(%s, (uint%d_t)x87_to_integer(c, %s, %d, %d));" %
                (op.size, addr_expr(op), op.size, get(0), op.size, m == "FISTTP")]
        if m != "FIST":
            body += ["fdrop(c);"]
    elif m in ("FADD", "FSUB", "FSUBR", "FMUL", "FDIV", "FDIVR",
               "FADDP", "FSUBP", "FSUBRP", "FMULP", "FDIVP", "FDIVRP",
               "FIADD", "FISUB", "FISUBR", "FIMUL", "FIDIV", "FIDIVR"):
        base = m[:-1] if m.endswith("P") else m
        kind = base.replace("FI", "F")[:4]
        operation = {"FADD": "ADD", "FSUB": "SUB", "FMUL": "MUL", "FDIV": "DIV"}[kind]
        dest, source = 0, None
        if m.endswith("P"):
            dest, source = ops[0].sti if ops else 1, get(0)
        elif ops[0].kind == "mem":
            source = integer(ops[0]) if m.startswith("FI") else load(ops[0])
        elif len(ops) == 2:
            dest, source = ops[0].sti, get(ops[1].sti)
        elif t.image.rd8(at) == 0xdc:
            dest, source = ops[0].sti, get(0)
        else:
            source = get(ops[0].sti)
        a, b = get(dest), source
        if base.endswith("R"):
            a, b = b, a
        body = [put(dest, "x87_binary(c, %s, %s, X87_%s)" % (a, b, operation))]
        if m.endswith("P"):
            body += ["fdrop(c);"]
    elif m in ("FCOM", "FCOMP", "FCOMPP", "FUCOM", "FUCOMP", "FUCOMPP",
               "FCOMI", "FCOMIP", "FUCOMI", "FUCOMIP", "FICOM", "FICOMP", "FTST"):
        other = get(1) if not ops else (get(ops[-1].sti) if ops[-1].kind == "st" else
                integer(ops[-1]) if m.startswith("FI") else load(ops[-1]))
        if m == "FTST":
            other = "x87_integer(0)"
        body = ["x87_compare(c, %s, %s, %d, %d);" % (get(0), other, m.startswith("FU"), "I" in m[2:])]
        body += ["fdrop(c);"] * (2 if m.endswith("PP") else 1 if m.endswith("P") else 0)
    elif m in ("FABS", "FCHS"):
        body = ["X87Value v_ = %s;" % get(0),
                "v_.signExp %s 0x%su;" % ("&=" if m == "FABS" else "^=", "7fff" if m == "FABS" else "8000"),
                put(0, "v_")]
    elif m in ("FSQRT", "FRNDINT", "FSIN", "FCOS", "FSINCOS", "FPTAN",
               "FPATAN", "F2XM1", "FYL2X", "FYL2XP1", "FPREM", "FPREM1"):
        operation = {"FSQRT": "SQRT", "FRNDINT": "ROUND", "FSIN": "SIN", "FCOS": "COS",
                     "FSINCOS": "SINCOS", "FPTAN": "TAN", "FPATAN": "ATAN", "F2XM1": "2XM1",
                     "FYL2X": "YL2X", "FYL2XP1": "YL2XP1", "FPREM": "PREM", "FPREM1": "PREM1"}[m]
        body = ["x87_unary(c, X87_%s);" % operation]
    elif m == "FSCALE":
        body = [put(0, "x87_binary(c, %s, %s, X87_SCALE)" % (get(0), get(1)))]
    elif m == "FXCH":
        body = ["fxch(c, %d);" % (ops[-1].sti if ops else 1)]
    elif m == "FXAM":
        body = ["fxam(c);"]
    elif m == "FFREE":
        body = ["ftag_put(c, c->fpu_top + %d, FTAG_EMPTY);" % ops[0].sti]
    elif m in ("FNSTSW", "FSTSW"):
        body = ["c->r[0] = (c->r[0] & 0xffff0000u) | fstsw(c);"] if ops[0].kind == "reg" else [
            "wr16(%s, fstsw(c));" % addr_expr(ops[0])]
    elif m in ("FNSTCW", "FSTCW"):
        body = ["wr16(%s, c->fpu_cw);" % addr_expr(ops[0])]
    elif m == "FLDCW":
        body = ["x87_set_cw(c, rd16(%s));" % addr_expr(ops[0])]
    elif m in ("FNINIT", "FINIT"):
        body = ["x87_finit(c);"]
    elif m in ("FNSAVE", "FSAVE", "FRSTOR", "FNSTENV", "FSTENV", "FLDENV"):
        helper = {"FNSAVE": "fnsave", "FSAVE": "fnsave", "FRSTOR": "frstor",
                  "FNSTENV": "fnstenv", "FSTENV": "fnstenv", "FLDENV": "fldenv"}[m]
        body = ["x87_%s(c, %s);" % (helper, addr_expr(ops[0]))]
    elif m in ("FCLEX", "FNCLEX"):
        body = ["c->fpu_sw &= (uint16_t)~0x80ffu; c->fpu_pending = 0;"]
    elif m == "FNOP":
        body = [";"]
    else:
        raise TranslateError("unqualified x87 form %s" % m)
    return lines + body
