"""CI-only publication of both verified installers; never overwrite an existing release."""

import os
from pathlib import Path
import subprocess
import tempfile

from release import ROOT, digest, validate_tag


def validated_assets(folder, tag):
    """Require both platform packages and their exact checksum inventories before publishing."""
    assets = []
    for system, suffix in (("windows-x64", ".exe"), ("linux-x64", ".tar.gz")):
        stem = f"GauntletDarkLegacy-{tag[1:]}-{system}-setup"
        checksum = folder / f"{stem}.sha256"
        expected = {f"{stem}{suffix}", f"{stem}-licenses.zip"}
        seen = set()
        for line in checksum.read_text(encoding="ascii").splitlines():
            sha, name = line.split("  ", 1)
            if name not in expected or name in seen or digest(folder / name) != sha:
                raise ValueError(f"Invalid release checksum: {name}")
            seen.add(name)
            assets.append(folder / name)
        if seen != expected:
            raise ValueError(f"Incomplete {system} release")
        assets.append(checksum)
    return assets


def main():
    tag = validate_tag(os.environ["RELEASE_TAG"])
    assets = validated_assets(ROOT / "release-assets", tag)
    notes = (
        "Alpha QA build — expect bugs and keep backups of your saves.\n\n"
        "Windows 10/11 x64: download and run the `windows-x64-setup.exe`. "
        "It is unsigned; only use downloads from this project's release page.\n\n"
        "Linux x64: extract the `linux-x64-setup.tar.gz`, then open the executable "
        "inside (or run it in a terminal). Baseline: Ubuntu 24.04+ desktop. "
        "Minimal desktops may need libegl1, libopengl0, libxkbcommon-x11-0 and libxcb-cursor0.\n\n"
        "Both: provide your own USA GameCube Gauntlet Dark Legacy ISO/CISO (GUNE5D), "
        "choose a writable installation folder, and press Install. No Python, compiler, "
        "console emulator or asset conversion is required. A Vulkan 1.3 graphics driver is required. "
        "No game assets are included in these downloads.\n\n"
        "The installer defaults to its own folder and will not overwrite an existing game. "
        "For an upgrade, install in a new folder, then copy your old `saves/` and `config/`. "
        "Settings and saves stay beside the executable. Check `VERSION`, `installation.json` "
        "or `gauntlet --version` when reporting a bug.\n\n"
        "Checksums and third-party licenses accompany both downloads.\n"
    )
    with tempfile.TemporaryDirectory(prefix="gdl-release-notes-") as temporary:
        path = Path(temporary) / "notes.txt"
        path.write_text(notes, encoding="utf-8")
        subprocess.run(["gh", "release", "create", tag, "--verify-tag", "--prerelease",
                        "--title", f"Gauntlet Dark Legacy {tag}", "--notes-file", str(path),
                        "--generate-notes", *map(str, assets)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
