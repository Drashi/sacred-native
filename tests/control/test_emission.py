"""Driver-owned checkpoints must not refer to generated native frames."""
import sys
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "third_party/recomp-kit/tools/recomp/tests"))
from test_translate_insns import Case, T, translate_case


def mismatch_case() -> Case:
    # This is the minimized listing that exposed the returned-checkpoint defect.
    return Case("seh_resumable_lifetime", 0x601000, [
        (0, "PUSH EBP"), (1, "PUSH 0x601080"),
        (6, "PUSH dword ptr FS:[0x0]"), (13, "MOV dword ptr FS:[0x0],ESP"),
        (20, "CALL EDX"), (22, "POP EAX"), (23, "POP ECX"), (24, "POP EBP"),
        (25, "MOV dword ptr FS:[0x0],EAX"), (31, "RET"),
    ], "55688010600064ff350000000064892500000000ffd25a595d64a300000000c3")


def test_driver_checkpoint_survives_mismatched_return_without_native_frame(monkeypatch) -> None:
    # Given: the original failing prologue, CALL, and ordinary unlink epilogue.
    monkeypatch.setattr(T, "RESUMABLE_STACKS", True)
    monkeypatch.setattr(T, "DRIVER_CHECKPOINTS", True, raising=False)
    # When: the adopted emitter lowers this function.
    text = translate_case(mismatch_case())
    # Then: registration is guest-only, even when CALL returns to another PC.
    assert "setjmp(" not in text
    assert "jmp_buf" not in text
    assert "recomp_seh_register(c)" in text
    assert "if (sacred_transfer_pending() || c->eip != 0x601016u) return;" in text
    assert "recomp_seh_frame_leave(c)" in text


@pytest.mark.parametrize(("address", "helper"), [
    (T.INTRINSIC_SETJMP, "recomp_guest_setjmp"),
    (T.INTRINSIC_LONGJMP, "recomp_guest_longjmp"),
])
def test_driver_intrinsics_never_use_generated_native_checkpoints(monkeypatch, address: int, helper: str) -> None:
    monkeypatch.setattr(T, "RESUMABLE_STACKS", True)
    monkeypatch.setattr(T, "DRIVER_CHECKPOINTS", True)
    text = translate_case(Case("intrinsic", address, [(0, "RET")], "c3"))
    assert f"{helper}(c);" in text
    assert "recomp_setjmp(c)" not in text and "recomp_longjmp(c)" not in text
