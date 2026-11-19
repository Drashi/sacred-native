"""The translation recipe of tools/lift/recipe.py, on a made-up export: it keeps the listing generate.py reads and
no instruction byte."""
import gzip
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/lift"))
import recipe  # noqa: E402

# (rva, bytes) in export order: two contiguous runs, a gap, and an out-of-order instruction after them.
LISTING = [(0x1000, "8b442404"), (0x1004, "c1e005"), (0x1007, "c3"),
           (0x1010, "55"), (0x1011, "8bec"), (0x1013, "e8aabbccdd"), (0x1018, "c3"),
           (0x2000, "0f1f840000000000"), (0x1020, "90")]
LEADERS = [0x1000, 0x1010, 0x1013, 0x2000]


def export(directory: Path, filename: str, image: bytes) -> None:
    directory.mkdir(parents=True)
    (directory / "manifest.json").write_text(json.dumps({"sha256": hashlib.sha256(image).hexdigest()}))
    with gzip.open(directory / "instructions.jsonl.gz", "wt") as stream:
        for rva, code in LISTING:
            stream.write(json.dumps({"address": {"module_id": filename, "rva": f"0x{rva:08x}"}, "bytes": code}) + "\n")

    with gzip.open(directory / "blocks.jsonl.gz", "wt") as stream:
        for rva in LEADERS:
            stream.write(json.dumps({"entries": [{"rva": f"0x{rva:08x}"}]}) + "\n")


def test_round_trip_keeps_the_listing_and_no_bytes(tmp_path):
    assets = tmp_path / "game"
    assets.mkdir()
    for name, filename, directory in recipe.MODULES:
        image = filename.encode() * 64
        (assets / filename).write_bytes(image)
        export(tmp_path / "export" / directory, filename, image)

    out = tmp_path / "recipe.json.gz"
    recipe.make(tmp_path / "export", out)
    loaded = recipe.load(out)
    for name, filename, directory in recipe.MODULES:
        file, listing = loaded[name]
        assert file == filename
        assert listing == recipe.from_export(tmp_path / "export" / directory)
        assert listing.instructions == tuple((rva, len(code) // 2) for rva, code in LISTING)
        assert listing.leaders == frozenset(LEADERS)

    text = gzip.decompress(out.read_bytes()).decode()
    for _, code in LISTING:
        if len(code) >= 6:  # shorter hex strings occur in any JSON by chance
            assert code not in text

    assert recipe.check(out, assets, tmp_path / "export") == 0


def test_check_rejects_another_version(tmp_path, capsys):
    assets = tmp_path / "game"
    assets.mkdir()
    for _, filename, directory in recipe.MODULES:
        (assets / filename).write_bytes(b"v1")
        export(tmp_path / "export" / directory, filename, b"v1")

    out = tmp_path / "recipe.json.gz"
    recipe.make(tmp_path / "export", out)
    (assets / "granny.dll").write_bytes(b"v2")
    assert recipe.check(out, assets, None) == 1
    assert "granny.dll: SHA-256" in capsys.readouterr().out
