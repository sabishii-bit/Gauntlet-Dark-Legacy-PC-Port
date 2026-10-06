#!/usr/bin/env python3
"""Audit native assets without exporting or modifying them.

    python scripts/asset_audit.py --verify
    python scripts/asset_audit.py --archive MONSTERS/DEM --lender LEVELS/LEVELB4

Builds the selected preset. The default scan eagerly decodes the corpus but does
not invent cross-archive load contexts. --verify additionally runs the native
behavior regressions and rejects skipped/empty runs. Reports remain under build/;
they are not game inputs or independent evidence of retail parity.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import xml.etree.ElementTree as ET

import build
import devenv

FILTER = ("[asset-conformance],[generator-lava],[g3-fountain],[g4-ghost-fence],"
          "[particle-attachment],[enemy-render-passes],[native-assets]~[unpacked]")

def require_complete_scan(report: pathlib.Path) -> None:
    """A successful process must supply current, nonempty machine-readable evidence."""
    result = json.loads(report.read_text(encoding="utf-8"))
    if (not isinstance(result, dict) or result.get("schema") != 1
            or not isinstance(result.get("archives"), list) or not result["archives"]
            or result.get("errors") != 0):
        raise ValueError("Native archive scan was empty, incomplete or reported errors")



def require_complete_tests(report: pathlib.Path) -> None:
    """Catch2 skips return success; a native validation gate must reject those."""
    root = ET.parse(report).getroot()
    cases = list(root.iter("testcase"))
    if not cases or any(case.find("skipped") is not None for case in cases):
        raise ValueError("Native conformance was empty or skipped tests; supply the game data")
    if any(case.find("failure") is not None or case.find("error") is not None for case in cases):
        raise ValueError("Native conformance contains failures")


def beneath(root: pathlib.Path, relative: str) -> pathlib.Path:
    """Keep user-selected archive/lender paths inside the supplied retail root."""
    path = (root / relative).resolve()
    if path != root and root not in path.parents:
        raise ValueError(f"Archive escapes the asset root: {relative}")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("preset", nargs="?", default=devenv.release_preset())
    parser.add_argument("--assets", type=pathlib.Path)
    parser.add_argument("--archive", help="one archive relative to the native root")
    parser.add_argument("--lender", action="append", default=[], help="relative lender, in runtime lookup order")
    parser.add_argument("--verify", action="store_true", help="also require non-skipped native behavior tests")
    args = parser.parse_args()
    presets = build.build_presets()
    if args.preset not in presets:
        parser.error(f"Unknown build preset: {args.preset}")
    if args.lender and not args.archive:
        parser.error("--lender requires --archive; a corpus has no single load context")
    if args.verify and args.archive:
        parser.error("--verify requires the full corpus, not a selected archive")
    binary = devenv.ROOT / "build" / presets[args.preset]
    fallback = pathlib.Path(os.environ.get("GDL_ASSET_DIR", str(devenv.ROOT / "assets/GUNE5D/Gauntlet")))
    assets = (args.assets or build.cache_path(binary, "GDL_ASSET_DIR", fallback)).resolve()
    if not assets.is_dir():
        parser.error(f"Game data not found: {assets}")
    try:
        archive = beneath(assets, args.archive) if args.archive else assets
        lenders = [beneath(assets, name) for name in args.lender]
        if args.verify:
            # Test asset roots are compile-time definitions, not environment overrides.
            devenv.run(["cmake", "--preset", presets[args.preset],
                       f"-DGDL_ASSET_DIR={assets}"])
        devenv.run([sys.executable, str(devenv.ROOT / "scripts/build.py"), args.preset])
        report = binary / "asset-audit.json"
        report.unlink(missing_ok=True)
        command = [str(binary / "bin" / f"assetcheck{build.EXE}"), str(archive), "--report", str(report)]
        if not args.archive:
            command += ["--recursive", "--decode-only"]
        for lender in lenders:
            command += ["--lender", str(lender)]
        scanned = subprocess.run(command, cwd=devenv.ROOT, check=False)
        print(f"Audit report: {report}")
        if scanned.returncode:
            return scanned.returncode
        require_complete_scan(report)
        if args.verify:
            tests_report = binary / "asset-conformance.xml"
            tests_report.unlink(missing_ok=True)
            checked = subprocess.run([str(binary / "bin" / f"tests{build.EXE}"), FILTER,
                                      "--reporter", "junit", "--out", str(tests_report)],
                                     cwd=devenv.ROOT, check=False)
            if checked.returncode:
                return checked.returncode
            require_complete_tests(tests_report)
            print(f"Native behavior checks passed without skips: {tests_report}")
        return 0
    except (ValueError, OSError, ET.ParseError) as error:
        print(f"Asset audit failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
