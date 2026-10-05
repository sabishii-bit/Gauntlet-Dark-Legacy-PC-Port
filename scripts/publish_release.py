"""CI-only publication of both verified installers; never overwrite an existing release."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from release import ROOT, SEMVER, digest, validate_tag


def alpha_order(tag):
    """Numeric precedence for the alpha-only versions accepted by the build."""
    value = tag.removeprefix("v")
    if not SEMVER.fullmatch(value):
        return None
    core, alpha = value.split("-alpha.")
    return (*map(int, core.split(".")), int(alpha))


def require_new_version(tag, existing_tags):
    """Published releases and drafts both reserve their version; never reuse or downgrade."""
    candidate = alpha_order(tag)
    if candidate is None:
        raise ValueError("A release requires an alpha semantic version")
    for existing in existing_tags:
        if existing == tag:
            raise ValueError(f"Release {tag} already exists; increment VERSION instead")
        previous = alpha_order(existing)
        if previous is not None and candidate <= previous:
            raise ValueError(f"Release {tag} must be newer than existing release {existing}")


def check_publication():
    """Fail closed before building and again before publishing, without mutating GitHub."""
    if os.environ.get("GITHUB_EVENT_NAME") != "push":
        raise ValueError("Only a pushed version tag may publish; manual runs are build-only")
    tag = validate_tag(os.environ["RELEASE_TAG"])
    if os.environ.get("GITHUB_REF") != f"refs/tags/{tag}":
        raise ValueError("Publication requires the matching version tag, not a branch")
    # --paginate includes old versions and drafts. Authentication/network failures abort,
    # rather than being mistaken for proof that the version has not been published.
    existing = subprocess.check_output(
        ["gh", "api", "--paginate", "repos/{owner}/{repo}/releases", "--jq", ".[].tag_name"],
        cwd=ROOT, text=True).splitlines()
    require_new_version(tag, existing)
    return tag


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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check eligibility without publishing")
    args = parser.parse_args(argv)
    tag = check_publication()
    if args.check:
        print(f"Eligible new release: {tag}")
        return
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
