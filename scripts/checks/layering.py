#!/usr/bin/env python3
"""Hold src/ to its layering: a module includes only the modules below it.

    python scripts/checks/layering.py [--root <repo>] [--tighten]

The engine's modules and the game's modules each stand in an order, lowest first. An engine
module includes only engine modules at or below its own; the game includes any engine
module but only game modules at or below its own. Neither the engine nor the native format
readers ever include the game, and the base game never names an optional expansion (the
expansions build on the game, not the reverse). The readers reach only the engine's value
layers. A mod under src/mods builds on the engine and the game alone: nothing else includes
it, and it includes no other mod and no expansion. The N64 and Dreamcast expansions build
on src/expansions/shared, the game and the engine, never on each other, and their tests
under tests/expansions keep the same boundary.

Breaks that predate this check are listed in layering_baseline.txt beside it, one
`<file> -> <module>` per line. A break not listed fails the check; a listed one that is
gone is reported so the list can shrink, and `--tighten` drops those entries. The list
only ever shrinks: a new break is fixed, not added.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE = pathlib.Path(__file__).resolve().with_name("layering_baseline.txt")

# Lowest first. `net` is the engine's packet transport, used only by the game's netplay.
ENGINE_ORDER = ("core", "math", "io", "net", "platform", "render", "codec", "audio", "assets",
                "ui", "world", "app")
# Lowest first. `widgets` are what every screen reads and draws with (menu input, fonts,
# option lists, scrolls); `mechanics` is the seam additive mechanics answer through, below
# everything that asks it; `hud` lies over the party, under the opponents, tower and shop that
# report through it; `context` is GameServices, what every screen reads; `console` is
# the developer console, below the scenes; `online` plays a room's stages over the scenes,
# which `netplay` keeps in step.
GAME_ORDER = ("config", "widgets", "combat", "mechanics", "characters", "players", "camera",
              "audio", "effects", "level", "party", "hud", "enemies", "critters", "bosses",
              "tower", "shop", "context", "menu", "rollback", "netplay", "console", "scenes",
              "online", "app")
# What the native format readers may include: the engine's value types, never its devices.
FORMATS_REACH = ("core", "math", "io", "render", "codec", "assets")
# The platform expansions; what both use lives in src/expansions/shared.
PLATFORMS = ("n64", "dreamcast")
SOURCE_SUFFIXES = (".h", ".cpp")
INCLUDE = re.compile(r'^\s*#\s*include\s+"(engine|formats|game|expansions|mods)/([A-Za-z0-9_]+)')


def module_of(relative: pathlib.PurePosixPath) -> tuple[str, str]:
    """The top folder under src/ and the module folder under it ("" for a top-level file)."""
    parts = relative.parts
    return parts[0], parts[1] if len(parts) > 2 else ""


def crosses_platforms(top: str, module: str, target_top: str, target: str) -> bool:
    """Whether a file of one platform expansion includes the other platform's code."""
    return (top == "expansions" and target_top == "expansions" and module in PLATFORMS
            and target in PLATFORMS and target != module)


def breaks_of(top: str, module: str, target_top: str, target: str) -> bool:
    """Whether an include from `top`/`module` of `target_top`/`target` climbs the layering."""
    if top == "expansions" and target_top == "expansions":
        return crosses_platforms(top, module, target_top, target)
    if target_top == "mods":
        return top != "mods" or target != module  # a mod's own folder alone
    if top == "mods":
        return target_top == "expansions"
    if top == "engine":
        if target_top in ("formats", "game", "expansions"):
            return target_top != "formats"
        return (target_top == "engine" and module in ENGINE_ORDER and target in ENGINE_ORDER
                and ENGINE_ORDER.index(target) > ENGINE_ORDER.index(module))
    if top == "formats":
        if target_top in ("game", "expansions"):
            return True
        return target_top == "engine" and target not in FORMATS_REACH
    if top == "game":
        if target_top == "expansions":
            return True
        return (target_top == "game" and module in GAME_ORDER and target in GAME_ORDER
                and GAME_ORDER.index(target) > GAME_ORDER.index(module))
    return False


def scan(root: pathlib.Path) -> set[str]:
    """Every `<file> -> <top>/<module>` include that climbs the layering: anywhere under src/,
    and under tests/expansions/, whose tests may include anything but the other platform."""
    found = set()
    for folder, judge in (("src", breaks_of), ("tests", crosses_platforms)):
        base = root / folder
        searched = base if folder == "src" else base / "expansions"
        for path in sorted(searched.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            relative = pathlib.PurePosixPath(path.relative_to(base).as_posix())
            top, module = module_of(relative)
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                match = INCLUDE.match(line)
                if match and judge(top, module, match.group(1), match.group(2)):
                    found.add(f"{folder}/{relative} -> {match.group(1)}/{match.group(2)}")
    return found


def read_baseline(path: pathlib.Path) -> set[str]:
    """The listed breaks, ignoring blank lines and `#` comments."""
    if not path.is_file():
        return set()
    entries = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        entry = line.split("#", 1)[0].strip()
        if entry:
            entries.add(entry)
    return entries


def write_baseline(path: pathlib.Path, entries: set[str]) -> None:
    header = ("# Includes that climb the layering, kept until fixed (scripts/checks/layering.py).\n"
              "# This list only shrinks: fix a new break rather than adding it here.\n")
    path.write_text(header + "".join(f"{entry}\n" for entry in sorted(entries)),
                    encoding="utf-8", newline="\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--baseline", type=pathlib.Path, default=BASELINE)
    parser.add_argument("--tighten", action="store_true",
                        help="drop listed breaks that are gone from the baseline")
    args = parser.parse_args(argv)
    found = scan(args.root)
    listed = read_baseline(args.baseline)
    new = sorted(found - listed)
    gone = sorted(listed - found)
    for entry in new:
        print(f"layering: new break {entry}", file=sys.stderr)
    for entry in gone:
        print(f"layering: fixed, drop from the baseline: {entry}")
    if args.tighten and gone:
        write_baseline(args.baseline, listed & found)
        print(f"layering: dropped {len(gone)} fixed entries")
    print(f"layering: {len(found)} known breaks, {len(new)} new")
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
