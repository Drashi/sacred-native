"""Prepare the known English Steam build in a separate, read-only game tree."""
import hashlib
import os
import shutil
import stat
import struct
import subprocess
import sys
from pathlib import Path

STEAM_HASH = "4df6659352282a0e57bdf69d2ac33396200fd0b54670d327cb9822ffdc4891cd"
CANONICAL_HASH = "db9b86a44c8c1cc8c5ed9e3f427121ab8b4ed62760861e58995f10af9d971e74"


def normalize(raw: bytes) -> bytes:
    if hashlib.sha256(raw).hexdigest() != STEAM_HASH:
        raise ValueError("Unsupported Steam executable")

    # File offsets belong to the exact SHA-256 above, a PE32 SteamStub 2.0 image.
    header = struct.unpack_from("<221I", raw, 0xb5f380)
    fields = [header[0], *(previous ^ current for previous, current in zip(header, header[1:]))]
    entry_va, _, code_size, key = fields[11:15]
    image = bytearray(raw[:0xb5e000])
    for offset in range(0x1000, 0x1000 + code_size, 4):
        (encoded,) = struct.unpack_from("<I", raw, offset)
        struct.pack_into("<I", image, offset, encoded ^ key)
        key = encoded

    (pe,) = struct.unpack_from("<I", raw, 0x3c)
    optional = pe + 24
    (optional_size,) = struct.unpack_from("<H", raw, pe + 20)
    (image_base,) = struct.unpack_from("<I", raw, optional + 28)
    struct.pack_into("<H", image, pe + 6, 4)
    struct.pack_into("<I", image, optional + 16, entry_va - image_base)
    struct.pack_into("<I", image, optional + 56, 0x196c000)
    struct.pack_into("<I", image, optional + 64, 0)
    # The removed .bind section header occupies the original header padding.
    padding = optional + optional_size + 4 * 40
    image[padding:0x1000] = bytes(0x1000 - padding)
    if hashlib.sha256(image).hexdigest() != CANONICAL_HASH:
        raise ValueError("Steam executable did not reconstruct to the supported Sacred.exe")
    return bytes(image)


def stage(assets: Path, target: Path, *, for_runtime: bool = False) -> Path:
    assets = assets.resolve()
    exe = assets / "Sacred.exe"
    if not exe.is_file():
        return assets
    raw = exe.read_bytes()
    if hashlib.sha256(raw).hexdigest() != STEAM_HASH:
        return assets

    image = normalize(raw)
    if for_runtime:
        # The runtime confines file access to the asset root, so symlinks to
        # the installation only work for the build step. Clone on APFS.
        target = target / "game"
        if sys.platform == "darwin":
            subprocess.run(["/bin/cp", "-cR", str(assets), str(target)], check=True)
        else:
            shutil.copytree(assets, target)
        # Some Steam folders are owner-read-only. Allow cleanup of the clone.
        for directory, _, _ in os.walk(target):
            path = Path(directory)
            path.chmod(path.stat().st_mode | stat.S_IWUSR)
        (target / "Sacred.exe").unlink()
    else:
        for entry in assets.iterdir():
            if entry.name != "Sacred.exe" and not entry.name.startswith("."):
                (target / entry.name).symlink_to(entry)

    (target / "Sacred.exe").write_bytes(image)
    return target


if __name__ == "__main__":
    print(stage(Path(sys.argv[1]), Path(sys.argv[2]), for_runtime=True))
