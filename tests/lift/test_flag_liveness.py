"""Region flag liveness and block ticks of tools/lift/generate.py.

Exactness rule under test: a flag store may be dropped only when every path from it inside the region overwrites
that flag (with an instruction whose emitted code writes it) before anything reads it, and every exit reads all."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/lift"))
import generate as G  # noqa: E402

T = G.T
ALL = T.ALL_FLAGS


def region(*listing: tuple[str, str]):
    """Instructions at consecutive 4-byte addresses from 0x1000; `listing` is (mnemonic, operands)."""
    insns = []
    for i, (mnemonic, operands) in enumerate(listing):
        ops = [o.strip() for o in operands.split(",")] if operands else []
        insns.append(T.Insn(0x1000 + 4 * i, mnemonic, None, ops, f"{0x1000 + 4 * i:08x}  {mnemonic} {operands}"))

    return insns, [ins.addr + 4 for ins in insns]


def test_overwritten_compare_is_dead_and_the_last_one_feeds_the_branch() -> None:
    insns, fall = region(("CMP", "EAX,0x1"), ("CMP", "EAX,0x2"), ("JE", "0x1010"), ("MOV", "EAX,0x0"),
                         ("MOV", "EBX,0x1"))
    live = G.region_liveness(insns, fall)
    assert live[0] == frozenset()
    # The JE target and fallthrough are in the region and write nothing before the region ends: all live.
    assert live[1] == ALL


def test_branch_to_a_flag_writer_keeps_only_what_the_branch_reads() -> None:
    insns, fall = region(("CMP", "EAX,0x1"), ("JE", "0x100c"), ("ADD", "EAX,0x1"), ("SUB", "EBX,0x1"),
                         ("RET", ""))
    live = G.region_liveness(insns, fall)
    assert live[0] == frozenset(("zf",))
    assert live[3] == ALL  # before RET everything leaves the region


def test_out_of_region_branch_and_exits_read_everything() -> None:
    insns, fall = region(("TEST", "EAX,EAX"), ("JNE", "0x9000"), ("ADD", "EAX,0x1"), ("XOR", "EAX,EAX"))
    live = G.region_liveness(insns, fall)
    assert live[0] == ALL
    assert live[3] == ALL  # last instruction of the region


def test_carry_chain_and_logic_ops_keep_what_they_do_not_write() -> None:
    insns, fall = region(("ADD", "EAX,ECX"), ("ADC", "EDX,EBX"), ("AND", "ESI,0xff"), ("CMP", "EAX,EDX"),
                         ("JB", "0x1000"))
    live = G.region_liveness(insns, fall)
    assert live[0] == frozenset(("cf",))    # ADC reads CF and rewrites the rest
    # AND writes every flag but AF, so the ADC's AF is overwritten only by the CMP: still dead.
    assert live[1] == frozenset()
    assert live[2] == frozenset()


def test_logic_ops_do_not_kill_af() -> None:
    insns, fall = region(("ADD", "EAX,ECX"), ("OR", "EAX,EAX"), ("PUSHFD", ""), ("CMP", "EAX,0x0"), ("RET", ""))
    live = G.region_liveness(insns, fall)
    assert live[0] == frozenset(("af",))    # PUSHFD sees the ADD's AF through the OR


def test_unknown_and_flag_reading_instructions_keep_everything() -> None:
    insns, fall = region(("SUB", "EAX,0x1"), ("LAHF", ""), ("CMP", "EAX,0x0"), ("RET", ""))
    assert G.region_liveness(insns, fall)[0] == ALL
    insns, fall = region(("SUB", "EAX,0x1"), ("FCMOVB", "ST0,ST1"), ("CMP", "EAX,0x0"), ("RET", ""))
    assert G.region_liveness(insns, fall)[0] == ALL
    insns, fall = region(("SUB", "EAX,0x1"), ("SETA", "AL"), ("CMP", "EAX,0x0"), ("RET", ""))
    assert G.region_liveness(insns, fall)[0] == frozenset(("cf", "zf"))


def test_neutral_instructions_pass_liveness_through() -> None:
    insns, fall = region(("CMP", "EAX,0x1"), ("MOV", "EBX,dword ptr [ESI]"), ("LEA", "ECX,[EBX + 0x4]"),
                         ("FLD", "float ptr [ESI]"), ("JA", "0x9000"))
    live = G.region_liveness(insns, fall)
    assert live[0] == ALL  # the JA leaves the region, so everything the CMP wrote stays live


def test_loop_back_edge_reaches_a_fixed_point() -> None:
    insns, fall = region(("DEC", "ECX"), ("ADD", "EAX,0x4"), ("CMP", "EAX,EDX"), ("JNE", "0x1000"),
                         ("RET", ""))
    live = G.region_liveness(insns, fall)
    # DEC writes all but CF; the loop head is reached again from JNE, so CF from the CMP is live through DEC/ADD.
    assert live[0] == frozenset()
    assert live[2] == ALL  # JNE falls into RET


def test_ticks_cover_every_instruction_once_and_split_at_transfers() -> None:
    insns, fall = region(("MOV", "EAX,0x1"), ("CMP", "EAX,0x0"), ("JE", "0x1010"), ("MOV", "EBX,0x2"),
                         ("ADD", "EBX,0x1"), ("CALL", "EAX"), ("MOV", "ECX,EAX"), ("RET", ""))
    ticks = G.tick_points(insns, fall, set())
    assert sum(ticks.values()) == len(insns)
    assert ticks == {0: 3, 3: 1, 4: 1, 5: 1, 6: 1, 7: 1}


def test_eip_store_kept_only_where_something_can_read_it() -> None:
    assert not G.needs_eip(["c->r[6] = rd32((c->r[4] + 0x1cu));  /* MOV ESI,dword ptr [ESP + 0x1c] */"])
    assert not G.needs_eip(["{   /* FMUL */", "    x87_set(c, 0, x87_binary(c, x87_get(c, 0), "
                            "x87_load(c, c->r[6], 32), X87_MUL));", "}"])
    assert not G.needs_eip(["c->r[0] = shl32_f(c, c->r[0], 3u);", "if (c->eflags_zf) { c->eip = 0x1000u; return; }"])
    assert G.needs_eip(["x87_wait(c);"])                       # raises the pending FPU exception at c->eip
    assert G.needs_eip(["idiv32(c, (uint32_t)(c->r[1]), 0x401000u);"])
    assert G.needs_eip(["c->eip = rd32(c->r[4]); c->r[4] += 4u; recomp_return(c); return;"])
    assert G.needs_eip(["recomp_seh_register(c);"])
