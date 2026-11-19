"""Resolution-dependent site table of tools/lift/resolution.py: expressions, operand rewrites, image checks and the
generated runtime table."""
import hashlib
import json
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/lift"))
import resolution as R  # noqa: E402

BASE = 0x400000


def table(tmp_path, code=(), data=(), raw=b"x"):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": hashlib.sha256(raw).hexdigest(), "cell_base": "0x401000",
                                "cell_end": "0x401040", "code": list(code), "data": list(data)}))
    return R.load(path)


def test_expressions_use_c_division_and_only_w_h():
    assert R.Expr.parse("W/2").value(1366, 768) == 683
    assert R.Expr.parse("(W - 1024) / 2 + 0x100").value(1366, 768) == 171 + 256
    assert R.Expr.parse("(1023 - W) / 2").value(1366, 768) == -171  # truncation toward zero, as in C
    assert R.Expr.parse("-W").value(1024, 768) == -1024
    for bad in ("X", "W ** 2", "W // 2", "f(W)", "True", "'a'", "W if H else 1", "W[0]"):
        with pytest.raises(R.TableError):
            R.Expr.parse(bad)

    assert R.Expr.parse("(W-1024)/2").c() == "(((int64_t)w - (int64_t)1024) / (int64_t)2)"
    # hd(on, off): the off value at the original size, the on value otherwise or when forced.
    size = R.Expr.parse("hd(0x2ef0f4, 0x970f4)")
    assert size.value(1024, 768) == 0x970f4 and size.value(1366, 768) == 0x2ef0f4
    assert size.value(1024, 768, hd=True) == 0x2ef0f4
    assert size.c() == "(hd ? (int64_t)3076340 : (int64_t)618740)"
    for bad in ("hd(1)", "hd(1, 2, 3)", "hd", "hd + 1", "max(W)", "abs(W)"):
        with pytest.raises(R.TableError):
            R.Expr.parse(bad)


def test_immediate_operand_becomes_a_cell_with_the_original_default(tmp_path):
    t = table(tmp_path, code=[{"address": "0x401100", "bytes": "6800040000", "value": "W"},
                              {"address": "0x401105", "bytes": "c744244c00030000", "operand": 1, "value": "H"},
                              {"address": "0x40110d", "bytes": "66c7442410ff03", "operand": 1, "type": "i16",
                               "value": "W-1"}])
    image = bytes(0x2000)
    assert R.rewrite(t, t.code[0x401100], ["0x400"], image, BASE) == ["dword ptr [0x401000]"]
    assert R.rewrite(t, t.code[0x401105], ["dword ptr [ESP + 0x4c]", "0x300"], image, BASE) == \
        ["dword ptr [ESP + 0x4c]", "dword ptr [0x401008]"]
    assert R.rewrite(t, t.code[0x40110d], ["word ptr [ESP + 0x10]", "0x3ff"], image, BASE)[1] == \
        "word ptr [0x401010]"
    # An expression that is not the original constant at 1024x768 is refused.
    bad = table(tmp_path, code=[{"address": "0x401100", "bytes": "6800040000", "value": "W+1"}])
    with pytest.raises(R.TableError, match="1024x768"):
        R.rewrite(bad, bad.code[0x401100], ["0x400"], image, BASE)


def test_absolute_memory_operand_points_at_a_float_cell(tmp_path):
    image = bytearray(0x2000)
    struct.pack_into("<f", image, 0x1800, 1024.0)
    t = table(tmp_path, code=[{"address": "0x401100", "bytes": "d90500184000", "type": "f32", "value": "W"}])
    assert R.rewrite(t, t.code[0x401100], ["dword ptr [0x401800]"], bytes(image), BASE) == ["dword ptr [0x401000]"]
    with pytest.raises(R.TableError, match="differs"):
        R.rewrite(t, t.code[0x401100], ["dword ptr [0x401804]"], bytes(image), BASE)

    with pytest.raises(R.TableError, match="absolute"):
        R.rewrite(t, t.code[0x401100], ["dword ptr [EAX + 0x401800]"], bytes(image), BASE)


def test_float_immediate_and_register_displacement(tmp_path):
    t = table(tmp_path, code=[
        {"address": "0x401100", "bytes": "c744241000008044", "operand": 1, "type": "f32", "value": "W"},
        {"address": "0x401200", "bytes": "8984d52cff0300", "operand": 0, "mode": "disp",
         "value": "hd(0x970f4, 0x3ff2c)"},
        {"address": "0x401300", "bytes": "8984d52cff0300", "operand": 0, "mode": "disp",
         "value": "hd(0x970f8, 0x3ff2c)"}])
    image = bytes(0x2000)
    assert R.rewrite(t, t.code[0x401100], ["dword ptr [ESP + 0x10]", "0x44800000"], image, BASE)[1] == \
        "dword ptr [0x401000]"
    with pytest.raises(R.TableError, match="0x44800000"):
        R.rewrite(t, t.code[0x401100], ["dword ptr [ESP + 0x10]", "0x44400000"], image, BASE)

    assert R.rewrite(t, t.code[0x401200], ["dword ptr [EBP + EDX*0x8 + 0x3ff2c]", "EAX"], image, BASE)[0] == \
        "dword ptr [EBP + EDX*0x8 + 0x3ff2c]@cell0x401008"
    with pytest.raises(R.TableError, match="no register-based"):
        R.rewrite(t, t.code[0x401200], ["dword ptr [0x3ff2c]", "EAX"], image, BASE)

    with pytest.raises(R.TableError, match="displacement is"):
        R.rewrite(t, t.code[0x401200], ["dword ptr [EBP + EDX*0x8 + 0x3ff30]", "EAX"], image, BASE)


def test_generator_turns_a_cell_displacement_into_an_address_expression():
    sys.path.insert(0, str(ROOT / "tools/lift"))
    import generate as G
    op = G.T.parse_operand("dword ptr [EBP + EDX*0x8 + 0x3ff2c]@cell0x1831810")
    assert op.kind == "mem" and op.disp == 0
    assert G.T.addr_expr(op) == "((c->r[5] + c->r[2] * 8u) + rd32(0x1831810u))"
    assert G.T.parse_operand("dword ptr [EBP + 0x10]").addr_c is None


def test_mode_only_read_redirects_a_memory_operand_with_the_mode_on(tmp_path):
    t = table(tmp_path, code=[{"address": "0x401100", "bytes": "d8642410", "operand": 0, "type": "f32",
                               "mode": "hd_read",
                               "value": "0"}])
    # One cell for the value, one shared flag cell (1 with the mode on).
    assert [(c.kind, c.expr.text) for c in t.cells] == [("f32", "0"), ("i32", "hd(1, 0)")]
    op = R.rewrite(t, t.code[0x401100], ["float ptr [ESP + 0x10]"], bytes(0x2000), BASE)[0]
    assert op == "float ptr [ESP + 0x10]@hdread0x401008,0x401000"
    sys.path.insert(0, str(ROOT / "tools/lift"))
    import generate as G
    parsed = G.T.parse_operand(op)
    assert G.T.addr_expr(parsed) == "(rd32(0x401008u) ? 0x401000u : (c->r[4] + 0x10u))"
    with pytest.raises(R.TableError, match="not a memory operand"):
        R.rewrite(t, t.code[0x401100], ["EAX"], bytes(0x2000), BASE)


def test_hooks_move_table_entries_with_the_mode_on(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "83c404", "adjust": [
            {"address": "0x402000", "value": "W-1024"}, {"address": "0x402008", "type": "i16", "value": "H-768"}]}]}))
    t = R.load(path)
    lines = R.hook_c(t, t.hooks[0x401100])
    assert lines[0].startswith("if (rd32(") and lines[-1] == "}"
    assert "wr32(0x402000u, rd32(0x402000u) + rd32(" in lines[1]
    assert "wr16(0x402008u, (uint16_t)(rd16(0x402008u) + rd32(" in lines[2]
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "83c404", "adjust": [{"address": "0x402000", "value": "W"}]}]}))
    with pytest.raises(R.TableError, match="not 0 at 1024x768"):
        R.load(path)


def test_before_hook_moves_an_outgoing_stack_argument(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "e8", "when": "before",
         "adjust": [{"address": 0, "base": "esp", "value": "(W-1024)/2"}]}]}))
    t = R.load(path)
    hook = t.hooks[0x401100]
    assert hook.before and hook.adjusts[0].esp
    assert "wr32(c->r[4] + 0x0u, rd32(c->r[4] + 0x0u) + rd32(" in R.hook_c(t, hook)[1]


def test_cells_are_shared_by_kind_and_expression(tmp_path):
    t = table(tmp_path, code=[{"address": "0x401100", "bytes": "68", "value": "W"},
                              {"address": "0x401200", "bytes": "68", "value": "W"},
                              {"address": "0x401300", "bytes": "68", "type": "f32", "value": "W"}])
    assert [c.address for c in t.cells] == [0x401000, 0x401008]


def test_image_checks(tmp_path):
    raw = b"\x00" * 16 + struct.pack("<I", 0x401010)
    image = bytearray(0x2000)
    struct.pack_into("<H", image, 0x1900, 0x2a8)
    t = table(tmp_path, data=[{"address": "0x401900", "bytes": "a802", "type": "i16", "value": "W-1024+0x2a8"}],
              raw=raw)
    with pytest.raises(R.TableError, match="refers to the cell area"):
        R.check_image(t, raw, bytes(image), BASE)

    clean = b"\x00" * 20
    t = table(tmp_path, data=[{"address": "0x401900", "bytes": "a802", "type": "i16", "value": "W-1024+0x2a8"}],
              raw=clean)
    R.check_image(t, clean, bytes(image), BASE)
    with pytest.raises(R.TableError, match="another executable"):
        R.check_image(t, b"other", bytes(image), BASE)

    image[0x1900] = 0
    with pytest.raises(R.TableError, match="data bytes"):
        R.check_image(t, clean, bytes(image), BASE)


def test_emitted_c_lists_cells_then_data(tmp_path):
    t = table(tmp_path, code=[{"address": "0x401100", "bytes": "68", "value": "W"},
                              {"address": "0x401200", "bytes": "d9", "type": "f32", "value": "H/2"}],
              data=[{"address": "0x401900", "bytes": "a802", "type": "i16", "value": "W-1024+0x2a8"}])
    text = R.emit_c(t, BASE)
    assert "{0x00401000u, 4u, 1u, 0u}" in text and "{0x00401008u, 4u, 1u, 0u}" in text and \
        "{0x00401900u, 2u, 0u, 0u}" in text
    assert "resolution_write_count = 3u" in text
    assert "case 1u: { float v_ = (float)((double)h / (double)2);" in text


def test_frame_rate_limit_variable():
    e = R.Expr.parse("100*F/60")
    assert e.uses_f and not R.Expr.parse("W/2").uses_f
    assert e.value(1024, 768) == 100 and e.value(1024, 768, f=120) == 200 and e.value(1024, 768, f=144) == 240
    assert R.Expr.parse("60/F").value(1024, 768, floating=True, f=120) == 0.5
    assert e.c() == "(((int64_t)100 * (int64_t)f) / (int64_t)60)"


def test_frame_rate_cells_are_flagged_and_fps_hooks_test_the_limit(tmp_path):
    t = table(tmp_path, code=[{"address": "0x401100", "bytes": "6a3c", "value": "F"}])
    text = R.emit_c(t, BASE)
    assert "{0x00401000u, 4u, 1u, 1u},  /* i32 F */" in text
    assert "int32_t hd, int32_t f)" in text and "case 0u: return (uint64_t)(int64_t)f;" in text
    path = tmp_path / "hooks.json"
    path.write_text(json.dumps({"sha256": "0" * 64, "cell_base": "0x401000", "cell_end": "0x401100",
                                "hooks": [{"address": "0x402000", "bytes": "d95c2418", "mode": "fps",
                                           "adjust": [{"type": "f32_carry", "base": "esp", "address": "0x18",
                                                       "value": "0.96", "slot": "clock"}]}]}))
    t = R.load(path)
    lines = "\n".join(R.hook_c(t, t.hooks[0x402000]))
    f_cell = t.cell_of[("i32", "F")]
    assert f"if (rd32({f_cell:#x}u) != 60u)" in lines
    assert "c->r[4] + 0x18u" in lines and "(int64_t)v_" in lines
    data = json.loads(path.read_text())
    data["hooks"][0]["mode"] = "hd"
    path.write_text(json.dumps(data))
    with pytest.raises(R.TableError, match="fps hooks"):
        R.load(path)


def test_shipped_table_loads():
    t = R.load(ROOT / "tools/lift/resolution_sites.json")
    assert t.sha256 == "db9b86a44c8c1cc8c5ed9e3f427121ab8b4ed62760861e58995f10af9d971e74"
    for site in t.code.values():
        assert site.expr.value(R.DEFAULT_W, R.DEFAULT_H, floating=site.kind.startswith("f")) is not None


def test_hook_moves_a_field_of_the_object_a_register_points_at(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "66", "adjust": [{"address": "0x1164", "type": "i16", "base": "esi",
                                                            "value": "W-1024"}]}]}))
    t = R.load(path)
    hook = t.hooks[0x401100]
    assert hook.adjusts[0].base == "esi" and not hook.adjusts[0].esp
    assert "wr16(c->r[6] + 0x1164u, (uint16_t)(rd16(c->r[6] + 0x1164u) + rd32(" in R.hook_c(t, hook)[1]


def test_hook_rejects_an_unknown_base(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "66", "adjust": [{"address": 0, "base": "si", "value": "W-1024"}]}]}))
    with pytest.raises(R.TableError, match="32-bit register"):
        R.load(path)


def test_hook_condition_on_the_object_a_register_points_at(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "inset": "0x401038",
                                "hooks": [{"address": "0x401100", "bytes": "53", "when": "before",
                                           "only_if": {"base": "ecx", "equals": "0x897078"},
                                           "adjust": [{"address": "0x401038", "value": "hd(1, 0)"}]}]}))
    t = R.load(path)
    assert t.inset == 0x401038
    line = R.hook_c(t, t.hooks[0x401100])[0]
    assert "&& rd32(c->r[1] + 0x0u) == 0x897078u" in line
    assert "resolution_inset_address = 0x00401038u" in R.emit_c(t, 0x400000)


def test_inset_word_is_kept_out_of_the_cells(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401010", "inset": "0x401000",
                                "code": [{"address": "0x401100", "bytes": "6800040000", "value": "W"}]}))
    with pytest.raises(R.TableError, match="exhausted"):
        R.load(path)


def test_fractional_constant_only_in_floating_cells():
    e = R.Expr.parse("0.01745329238474369*W/1024")
    assert R.encode("f32", e.value(1024, 768, floating=True)) == bytes.fromhex("35fa8e3c")
    assert "(double)0.01745329238474369" in e.c(True)
    with pytest.raises(R.TableError, match="floating-point cell"):
        e.value(1024, 768)


def test_hook_scales_a_float(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "f3", "adjust": [{"address": "0x5d4", "type": "f32_scale", "base": "ebx",
                                                          "value": "1024/W"}]}]}))
    t = R.load(path)
    assert ("f32", "1024/W") in t.cell_of
    assert "f_ *= s_" in "\n".join(R.hook_c(t, t.hooks[0x401100]))
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "f3", "adjust": [{"address": 0, "type": "f32_scale", "value": "W/1000"}]}]}))
    with pytest.raises(R.TableError, match="not 1"):
        R.load(path)


def test_hook_adds_to_a_float(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "f3", "adjust": [{"address": "0x5f4", "type": "f32_add", "base": "ebx",
                                                          "value": "(1024-W)/W"}]}]}))
    t = R.load(path)
    assert "f_ += s_" in "\n".join(R.hook_c(t, t.hooks[0x401100]))


def test_menu_canvas_extension():
    # D: half the extra width of a 768-high canvas with the window's aspect, 0 for 4:3 or narrower and below 768
    # rows, at most 406 (a 1836-pixel canvas).
    assert R.menu_extension(1024, 768) == 0 and R.menu_extension(1280, 1024) == 0
    assert R.menu_extension(1366, 768) == 171 and R.menu_extension(1920, 1080) == 170
    assert R.menu_extension(2560, 1080) == 398 and R.menu_extension(3440, 1440) == 405
    assert R.menu_extension(5120, 1440) == 406 and R.menu_extension(1366, 720) == 0
    assert R.Expr.parse("(1024+2*D-W)/W").value(1920, 1080, floating=True) == (1024 + 340 - 1920) / 1920
    c = R.emit_c(R.Table("00", 0, 0, {}, [], [], {}), 0x400000)
    assert "const int32_t d = h < 768" in c


def test_hook_raises_flags(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "f3",
         "adjust": [{"address": "0x401038", "type": "or", "value": "hd(257, 0)"}]}]}))
    t = R.load(path)
    assert ("i32", "hd(257, 0)") in t.cell_of
    assert "wr32(0x401038u, rd32(0x401038u) | rd32(" in "\n".join(R.hook_c(t, t.hooks[0x401100]))
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "f3", "adjust": [{"address": "0x401038", "type": "or", "value": "1"}]}]}))
    with pytest.raises(R.TableError, match="not 0"):
        R.load(path)


def test_min_and_max():
    e = R.Expr.parse("min(768 - H, 0)")
    assert e.value(1024, 720) == 0 and e.value(1366, 800) == -32 and e.value(1024, 768) == 0
    assert R.Expr.parse("max(H - 768, 0) / 2").value(1366, 720) == 0
    assert "<" in e.c()


def test_hook_moves_a_register(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "83", "when": "before",
         "adjust": [{"register": "eax", "value": "(W-1024)/2"}]}]}))
    t = R.load(path)
    assert "c->r[0] += rd32(" in "\n".join(R.hook_c(t, t.hooks[0x401100]))
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "83", "adjust": [{"register": "esp", "value": "W-1024"}]}]}))
    with pytest.raises(R.TableError, match="register adjustment"):
        R.load(path)


def test_save_and_restore_bracket_a_call_under_an_absolute_condition(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "90", "when": "before", "only_if": {"address": "0x401038", "equals": 1},
         "adjust": [{"type": "save", "slot": "m", "base": "edi", "address": "0x10"},
                    {"type": "f32_scale", "base": "edi", "address": "0x10", "value": "1024/W"}]},
        {"address": "0x401110", "bytes": "90", "when": "before", "only_if": {"address": "0x401038", "equals": 1},
         "adjust": [{"type": "restore", "slot": "m", "base": "edi", "address": "0x10"}]}]}))
    t = R.load(path)
    save, restore = R.hook_c(t, t.hooks[0x401100]), R.hook_c(t, t.hooks[0x401110])
    assert "rd32(0x401038u) == 0x1u" in save[0] and "rd32(0x401038u) == 0x1u" in restore[0]
    slot = t.cell_of[("slot", "m")]
    assert save[1] == f"    wr32({slot:#x}u, rd32(c->r[7] + 0x10u));"
    assert restore[1] == f"    wr32(c->r[7] + 0x10u, rd32({slot:#x}u));"
    # The scratch word is its own cell, never a value cell shared with an expression.
    assert slot != t.cell_of[("f32", "1024/W")]


def test_condition_reads_a_16_bit_field_against_a_list_of_values(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "hooks": [
        {"address": "0x401100", "bytes": "90", "when": "before",
         "only_if": {"base": "edi", "address": "0x1160", "type": "i16", "equals": ["0x11", "0x14"]},
         "adjust": [{"type": "save", "slot": "k", "base": "edi", "address": "0x1164"}]},
        {"address": "0x401110", "bytes": "90", "when": "before",
         "only_if": {"base": "edi", "address": "0x1160", "type": "i8", "equals": 1},
         "adjust": [{"type": "restore", "slot": "k", "base": "edi", "address": "0x1164"}]}]}))
    with pytest.raises(R.TableError, match="i32 or i16"):
        R.load(path)

    raw = json.loads(path.read_text())
    raw["hooks"].pop()
    path.write_text(json.dumps(raw))
    t = R.load(path)
    expected = "&& (rd16(c->r[7] + 0x1160u) == 0x11u || rd16(c->r[7] + 0x1160u) == 0x14u)"
    assert expected in R.hook_c(t, t.hooks[0x401100])[0]


def test_option_sets_a_register_while_its_setting_is_on(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "options": [
        {"setting": "LEGACY_UI", "address": "0x401100", "bytes": "32c0", "register": "eax", "value": 1}]}))
    t = R.load(path)
    option = t.options[0x401100]
    assert R.option_c(option) == [f"if (rd32({option.cell:#x}u)) {{  /* LEGACY_UI option 0x00401100 */",
                                  "    c->r[0] = 0x1u;", "}"]
    text = R.emit_c(t, BASE)
    assert "resolution_option_count = 1u;" in text
    assert f'{{"LEGACY_UI", 0x{option.cell:08x}u}},' in text
    # The switch word is the runtime's: it is not among the cells written from the screen size.
    assert f"0x{option.cell:08x}u, 4u, 1u" not in text
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "options": [
        {"setting": "LEGACY_UI", "address": "0x401100", "bytes": "32c0", "register": "eax", "value": 1,
         "skip_returns": ["0x6ac93d"]},
        {"setting": "LEGACY_UI", "address": "0x401200", "bytes": "682c1a0000", "base": "esp", "value": "0x196a"}]}))
    t = R.load(path)
    pushed = t.options[0x401200]
    assert pushed.cell == t.options[0x401100].cell  # one switch word per setting
    assert R.option_c(pushed)[1] == "    wr32(c->r[4] + 0x0u, 0x196au);"
    assert "&& rd32(c->r[4]) != 0x6ac93du)" in R.option_c(t.options[0x401100])[0]
    assert R.emit_c(t, BASE).count('"LEGACY_UI"') == 1
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401080", "options": [
        {"setting": "LEGACY_UI", "address": "0x402000", "bytes": "0000000011111111", "table": ["A.tga", "BC.tga"]}]}))
    t = R.load(path)
    (start, blob), = t.blobs
    write, = t.option_writes
    assert start % 8 == 0 and len(blob) % 8 == 0 and blob[:13] == b"A.tga\0BC.tga\0"
    assert write.address == 0x402000 and write.value == start.to_bytes(4, "little") + (start + 6).to_bytes(4, "little")
    text = R.emit_c(t, BASE)
    assert "resolution_blob_count = 1u;" in text and "resolution_option_write_count = 1u;" in text
    assert text.count('"LEGACY_UI"') == 2  # the write and its switch
    path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401080", "options": [
        {"setting": "LEGACY_UI", "address": "0x402000", "bytes": "61625f6300", "text": "abc"}]}))
    write, = R.load(path).option_writes
    assert write.value == b"abc\0\0"
    with pytest.raises(R.TableError, match="does not fit"):
        path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401080", "options": [
            {"setting": "LEGACY_UI", "address": "0x402000", "bytes": "61625f6300", "text": "abcde"}]}))
        R.load(path)

    with pytest.raises(R.TableError, match="one string per original pointer"):
        path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401080", "options": [
            {"setting": "LEGACY_UI", "address": "0x402000", "bytes": "00000000", "table": ["A.tga", "B.tga"]}]}))
        R.load(path)

    for bad in ({"setting": "legacy", "register": "eax"}, {"setting": "LEGACY_UI", "base": "esp", "register": "eax"},
                {"setting": "LEGACY_UI", "register": "esp"},
                {"setting": "LEGACY_UI", "register": "al"}):
        path.write_text(json.dumps({"sha256": "00", "cell_base": "0x401000", "cell_end": "0x401040", "options": [
            {"address": "0x401100", "bytes": "32c0", "value": 1, **bad}]}))
        with pytest.raises(R.TableError):
            R.load(path)


def test_length_entries_take_their_bytes_from_the_image(tmp_path):
    path = tmp_path / "sites.json"
    path.write_text(json.dumps({"sha256": "0" * 64, "cell_base": "0x401000", "cell_end": "0x401040",
                                "code": [{"address": "0x401100", "length": 5, "value": "W"}]}))
    image = bytearray(0x2000)
    image[0x1100:0x1105] = bytes.fromhex("6800040000")
    assert R.load(path, bytes(image), BASE).code[0x401100].original == bytes.fromhex("6800040000")
    assert R.load(path).code[0x401100].original == bytes(5)
    with pytest.raises(R.TableError, match="outside the image"):
        R.load(path, bytes(0x1000), BASE)
