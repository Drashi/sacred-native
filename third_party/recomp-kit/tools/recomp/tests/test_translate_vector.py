"""AVX is a trap in place, as the rest of the unmodelled vector set is.

Delphi's Move has an AVX path beside its SSE one, chosen by a CPUID test this
kit's recomp_cpuid never passes, so it is never run. The translator could not
parse a YMM operand, though, and refused the whole function over it; Move is
called from everywhere, so Siege of Avalon's build reported 404 undeliverable
calls, all but one of them to that single function.
"""
from test_translate_insns import Case, T, translate_case
import pytest

BASE = 0x0D02F400


def test_an_avx_instruction_becomes_a_trap_rather_than_losing_its_function():
    case = Case("avx_in_move", BASE,
                [(0, "VMOVUPS YMM1,ymmword ptr [EAX]"), (4, "RET")],
                "c5 fc 10 08 c3")
    text = translate_case(case)  # no TranslateError
    assert "recomp_unmodelled(c, %s)" % T.hexlit(BASE) in text


def test_the_vector_rule_names_ymm_registers_and_ymmword_operands():
    assert T.is_vector_insn("VMOVUPS", ["YMM1", "ymmword ptr [EAX]"])
    assert T.is_vector_insn("VPXOR", ["YMM0", "YMM0", "YMM0"])
    assert not T.is_vector_insn("MOV", ["EAX", "dword ptr [EBX]"])


@pytest.mark.parametrize("instruction,code", [
    ("PACKSSWB XMM0,XMM1", "66 0f 63 c1"),
    ("PSLLD XMM0,XMM1", "66 0f f2 c1"),
])
def test_unqualified_packed_forms_remain_fatal_quarantine(instruction, code):
    """MMX half-lane helpers must not admit unqualified 128-bit forms."""
    size = len(bytes.fromhex(code))
    case = Case("unqualified_packed", BASE,
                [(0, instruction), (size, "RET")], code + " c3")
    text = translate_case(case)
    assert "recomp_unmodelled(c, %s)" % T.hexlit(BASE) in text
