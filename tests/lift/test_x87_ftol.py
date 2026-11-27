"""CALL _ftol as an operation of a native-double x87 run (tools/lift/x87_fast.py, Compiler.call).

Under test: the run takes the call in and goes on behind it into the next block; it leaves the run in front of the
call's own tick, and behind the tick of the block it entered; the verify mode's run holds the call alone."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/lift"))
import generate as G  # noqa: E402
import x87_fast as X  # noqa: E402

T = G.T
FTOL = X.Ftol(fistp=0x2019, opcode=0x77d, ret=0x2026, length=15)
# FLD m32 [EBX+12]; FADD m32 [EDI]; PUSH 0; CALL ftol; MOV [ESP],EAX; FLD m32 [EBX+8]; FADD m32 [EDI-4]; RET
LISTING = ((0x1000, "FLD", "float ptr [EBX + 0xc]", "d9430c"), (0x1003, "FADD", "float ptr [EDI]", "d807"),
           (0x1005, "PUSH", "0x0", "6a00"), (0x1007, "CALL", "0x2000", "e8f40f0000"),
           (0x100c, "MOV", "dword ptr [ESP],EAX", "890424"), (0x100f, "FLD", "float ptr [EBX + 0x8]", "d94308"),
           (0x1012, "FADD", "float ptr [EDI + -0x4]", "d847fc"), (0x1015, "RET", "", "c3"))


# The same without the instructions between: the block behind the call starts with an x87 instruction.
SHORT = ((0x1000, "FLD", "float ptr [EBX + 0xc]", "d9430c"), (0x1003, "CALL", "0x2000", "e8f80f0000"),
         (0x1008, "FLD", "float ptr [EBX + 0x8]", "d94308"), (0x100b, "FADD", "float ptr [EDI]", "d807"),
         (0x100d, "RET", "", "c3"))


class Image:
    def __init__(self, listing) -> None:
        self.bytes = {address + i: byte for address, _, _, code in listing
                      for i, byte in enumerate(bytes.fromhex(code))}

    def rd8(self, address: int) -> int:
        return self.bytes[address]


def compile_from(start: int, limit: int, calls, verify: bool = False, listing=LISTING, exit_lines=None):
    insns = [T.Insn(a, m, None, [o] if o else [], f"{a:08x}  {m} {o}") for a, m, o, _ in listing]
    emitted = [[f"/* {ins.raw} */"] for ins in insns]
    return X.compile_run(Image(listing), insns, emitted, start, limit, lambda k: insns[k].mnem in ("PUSH", "MOV"),
                         exit_lines or (lambda k: [f"goto L_{insns[k + 1].addr:08x};"]), calls, verify)


def test_a_run_takes_the_call_in_and_goes_on_into_the_block_behind_it() -> None:
    end, body = compile_from(0, 7, {3: X.Call(FTOL, 0x100c, 3)})
    text = "\n".join(body)
    assert end == 6
    # What the routine leaves: the result, its stack bytes, the flags, EIP, source and budget of the call.
    assert "c->r[0] = (uint32_t)(uint64_t)" in text and "c->r[2] = (uint32_t)((uint64_t)" in text
    assert "wr32(v7_ - 4u, 0x0000100cu); wr32(v7_ - 8u, c->r[5]); wr16(v7_ - 10u, c->fpu_cw);" in text
    assert "wr16(v7_ - 12u, (uint16_t)(c->fpu_cw | 0xc00u)); wr64(v7_ - 20u, (uint64_t)v8_);" in text
    assert "c->eflags_cf = 0; c->eflags_of = 0; c->eflags_zf = 0;" in text
    # The call's tick and the routine's 15 instructions, then the tick of the block the run enters.
    assert "c->eip = 0x0000100cu; c->source_pc = 0x00002026u; c->budget += 16u;" in text
    assert "c->budget += 3u;" in text
    # The routine's FISTP is the last x87 instruction pointer only if nothing follows; here the FADD is.
    assert "c->fpu_ip = 0x00001012u;" in text


def test_the_run_is_left_in_front_of_the_calls_tick_and_behind_the_next_blocks() -> None:
    _, body = compile_from(0, 7, {3: X.Call(FTOL, 0x100c, 3)})
    call = next(line for line in body if "x87f_ftol_ok(" in line)
    assert "x87f_ftol_ok(v6_, 1u) && recomp_direct_enabled" in call and call.rstrip().endswith("goto L_00001007; }")
    behind = [line for line in body if "T_0000100f" in line or "T_0000100c" in line]
    # 0x100c (MOV) starts the block behind the call but cannot fail; the FLD behind it is inside that block.
    assert not behind
    assert any("x87f_ok_f32" in line and "goto L_0000100f; }" in line for line in body)


def test_an_x87_instruction_right_behind_the_call_resumes_behind_its_tick() -> None:
    end, body = compile_from(0, 4, {1: X.Call(FTOL, 0x1008, 2)}, listing=SHORT)
    assert end == 3
    assert "c->budget += 2u;" in body
    resume = next(line for line in body if "T_00001008" in line)
    assert "x87f_ok_f32" in resume and resume.rstrip().endswith("c->eip = 0x00001008u; goto T_00001008; }")


def test_a_call_that_ends_the_run_leaves_the_next_tick_to_its_label() -> None:
    _, body = compile_from(0, 4, {3: X.Call(FTOL, 0x100c, 3)})
    assert body[-3:] == ["goto L_0000100c;", "}", "F_00001000: ;"]
    assert "c->budget += 16u;" in "\n".join(body) and "c->budget += 3u;" not in body


def test_a_call_alone_is_a_run_and_counts_only_the_routine() -> None:
    end, body = compile_from(3, 4, {3: X.Call(FTOL, 0x100c, 3)})
    text = "\n".join(body)
    assert end == 3
    assert "x87f_enter(c, 0x2u, 0x0u)" in text                   # ST0 full
    assert "c->budget += 15u;" in text and "goto F_00001007; }" in text
    assert "c->fpu_ip = 0x00002019u; c->fpu_opcode = 0x77d;" in text
    assert "x87f_put(c, t0_ + 0u, e0_, 0);" in text and "c->fpu_top = (t0_ + 1u) & 7u;" in text


def test_without_the_lowering_the_call_ends_the_run() -> None:
    end, _ = compile_from(0, 3, {})
    assert end == 1
    assert compile_from(3, 4, {}) is None


def test_the_verify_run_holds_the_call_alone_and_hands_its_result_over() -> None:
    end, body = compile_from(3, 4, {3: X.Call(FTOL, 0x100c, 3)}, verify=True,
                             exit_lines=lambda k: ["x87f_ftol_verify_inline(c);"])
    assert end == 3
    assert body[:2] == ["if (RECOMP_UNLIKELY(x87f_ftol_mode == 2u)) {", "x87f_ftol_verify_begin(c);"]
    assert body[-4:] == ["x87f_ftol_verify_inline(c);", "}", "V_00001007: ;", "}"]
    assert any("x87f_ftol_ok(e0_, 2u)" in line and "goto V_00001007; }" in line for line in body)


def test_c1_of_the_call_is_written_where_something_reads_it() -> None:
    insns = [T.Insn(a, m, None, [o] if o else [], "") for a, m, o, _ in LISTING]
    # Offsets 0..3 of the run FLD, FADD, PUSH, CALL: the call is the last C1 writer, read at the end of the run.
    assert X.observed_writers(insns[:4], frozenset({3})) == {0: False, 1: False, 3: True}
    assert X.observed_writers(insns[:4]) == {0: False, 1: True}
