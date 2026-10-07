"""CI-only publication of both verified installers; never overwrite an existing release."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from release import ROOT, digest, validate_tag
from installer.install import payload_inventory
from installer.releases import runtime_name
from installer.versions import is_prerelease, version_order


def require_new_version(tag, existing_tags):
    """Published releases and drafts both reserve their version; never reuse or downgrade."""
    candidate = version_order(tag)
    if candidate is None:
        raise ValueError("A release requires a semantic version")
    for existing in existing_tags:
        if existing == tag:
            raise ValueError(f"Release {tag} already exists; increment VERSION instead")
        previous = version_order(existing)
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
        expected = {f"{stem}{suffix}", f"{stem}-licenses.zip", runtime_name(tag[1:], system)}
        seen = set()
        for line in checksum.read_text(encoding="ascii").splitlines():
            sha, name = line.split("  ", 1)
            if name not in expected or name in seen or digest(folder / name) != sha:
                raise ValueError(f"Invalid release checksum: {name}")
            seen.add(name)
            assets.append(folder / name)
        if seen != expected:
            raise ValueError(f"Incomplete {system} release")
        metadata, _ = payload_inventory(folder / runtime_name(tag[1:], system))
        executable = "gauntlet.exe" if system == "windows-x64" else "gauntlet"
        if (metadata["version"] != tag[1:] or metadata["platform"] != system or
                metadata["executable"] != executable):
            raise ValueError(f"Runtime inventory disagrees with the {system} release")
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
    prerelease = is_prerelease(tag)
    heading = ("Prerelease QA build — expect bugs and keep backups of your saves.\n\n"
               if prerelease else "Keep backups of your saves before updating.\n\n")
    notes = (
        heading +
        "Windows 10/11 x64: download and run the `windows-x64-setup.exe`. "
        "It is unsigned; only use downloads from this project's release page.\n\n"
        "Linux x64: extract the `linux-x64-setup.tar.gz`, then open the executable "
        "inside (or run it in a terminal). Baseline: Ubuntu 24.04+ desktop. "
        "Minimal desktops may need libegl1, libopengl0, libxkbcommon-x11-0 and libxcb-cursor0.\n\n"
        "Both: provide your own USA GameCube Gauntlet Dark Legacy ISO/CISO (GUNE5D), "
        "choose a writable installation folder, and press Install. No Python, compiler, "
        "console emulator or asset conversion is required. A Vulkan 1.3 graphics driver is required. "
        "No game assets are included in these downloads.\n\n"
        "For an upgrade, open the installer and select your existing game folder. "
        "It checks all published releases, including prereleases; press Update to download and install the newer game and installer. "
        "Close the installer window when finished so it can replace itself in place. "
        "No disc image is needed again. Close the game first. "
        "Game assets, `saves/` and `config/` are preserved. Fresh installations also retain "
        "a GauntletDarkLegacy-Update executable beside the game for future checks. "
        "Settings and saves stay beside the executable. Check `VERSION`, `installation.json` "
        "or `gauntlet --version` when reporting a bug.\n\n"
        "Checksums and third-party licenses accompany both downloads.\n"
    )
    with tempfile.TemporaryDirectory(prefix="gdl-release-notes-") as temporary:
        path = Path(temporary) / "notes.txt"
        path.write_text(notes, encoding="utf-8")
        subprocess.run(["gh", "release", "create", tag, "--verify-tag", *(["--prerelease"] if prerelease else []),
                        "--title", f"Gauntlet Dark Legacy {tag}", "--notes-file", str(path),
                        "--generate-notes", *map(str, assets)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
