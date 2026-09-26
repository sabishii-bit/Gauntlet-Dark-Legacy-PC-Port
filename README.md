# Gauntlet Dark Legacy

A reconstruction of *Gauntlet Dark Legacy* (GameCube release, `GUNE5D`) in
C++26 on Vulkan, written to be readable, modular and easy to extend. It reads
the original game data; no assets are included, you need your own copy of the
disc.

## Quick start

```
git clone https://github.com/sabishii-bit/Gauntlet-Dark-Legacy-PC-Port.git
cd Gauntlet-Dark-Legacy-PC-Port
python scripts/setup.py              # finds or installs the tools, then builds
```

Then extract your disc into `assets/GUNE5D` as [assets/README.md](assets/README.md)
describes, convert the console files once, and play:

```
python scripts/build.py --unpack --levels
python scripts/build.py --run
```

`--run` defaults to the platform's Release build for playtesting. Builds and
tests without `--run` default to Debug; an explicit preset overrides either.

Python 3.9+ and git are all `setup.py` needs to begin with. It reports what it
found, asks before installing anything (`--yes` skips the questions, `--check`
only reports), and ends with a built `build/<preset>/bin/gauntlet`.

## What it installs

| | Windows | Linux |
| --- | --- | --- |
| Compiler | Visual Studio 2022 Build Tools, "Desktop development with C++" (winget) | GCC 14+ or Clang 17+ from apt, dnf or pacman |
| CMake 3.30+ and Ninja | bundled with the C++ workload | the distribution's, or from pip when too old |
| vcpkg | cloned to `C:\vcpkg` | cloned to `~/vcpkg` |
| Windowing and Vulkan | nothing to install | X11, Wayland, GL and Vulkan development packages |
| Dependency build tools | supplied by vcpkg | Autoconf, autoconf-archive, Automake and libtool |
| Editor tooling (`--tooling`) | LLVM (clangd, clang-tidy, clang-format) | the same from the distribution |

A Vulkan 1.3 capable graphics driver is checked for but comes with your GPU
vendor's driver (Mesa 22+ on Linux). The libraries themselves (GLFW, GLM, volk,
Vulkan Memory Allocator, glslang, Catch2, miniaudio, nlohmann-json, stb) are
built by vcpkg on the first configure, which takes a few minutes once.

Set `VCPKG_ROOT` if your vcpkg checkout lives elsewhere. On Windows the
scripts enter an x64 MSVC developer environment themselves and prefer the
CMake and Ninja bundled with Visual Studio; an MSYS2 or Cygwin CMake, or a
`ninja.bat` shim, on `PATH` cannot drive MSVC, so keep a native one first.

## Building

```
python scripts/configure.py [preset] [--fresh]     # configure (default: the platform's Debug Ninja preset)
python scripts/build.py [preset] [--test] [--unpack [--levels]] [--run -- <game arguments>]
python scripts/devenv.py [--shell]                 # print the tools the scripts found, or open a shell with them
```

`cmake --list-presets` shows the presets for your platform:
`windows-ninja-debug` / `windows-ninja-release` (Ninja + MSVC),
`windows-vs2022` (a Visual Studio solution under `build/windows-vs2022/`),
`linux-ninja-debug` / `linux-ninja-release` (Ninja, the default compiler) and
`linux-clang-debug` / `linux-clang-release`. Each builds into
`build/<configure-preset>/` with the executables, the compiled shaders and (on
Windows) the dependency DLLs under `bin/`.

## Running

`gdlunpack` converts the console files under `assets/GUNE5D` into PNG, OBJ,
WAV and JSON under `assets/unpacked` (about 200 MB; the level folders add
about 20 MB each and come with `--levels`). `python scripts/build.py --unpack`
runs it for you (the level folders and `MONSTERS` are opt-in);
`gdlunpack <assets> <out> --only <folder>` converts one
folder. Existing exports need `--only MAPS` and `--only WDATA` for the
world-map/route and stage-preview travel screens. Movies still use the original
`VQMOVIES` files. `python scripts/scenario.py tower-portal-g1` starts on the
Fields portal to test departure, both loading screens, and the first-visit movie.

```
gauntlet [--assets <dir>] [--unpacked <dir>] [--data <dir>] [--title] [--movie <name>]
         [--scenario <file>] [--frames <n>] [--no-vsync] [--validation | --no-validation]
```

`--title` skips the intro movies, `--movie <name>` plays one movie and quits,
`--frames <n>` quits after that many frames, and `--scenario <file>` opens the
tower straight onto a described party and place (see `tests/scenarios/`).

For a shorter scenario command:

```sh
python scripts/scenario.py genie                 # launch the existing Release build
python scripts/scenario.py --list                # show every scenario
python scripts/scenario.py dragon --build        # build first, then launch
python scripts/scenario.py genie --frames 600    # bounded smoke test
```

Use a full scenario name, a unique suffix (`genie`, `dragon`, `lich`), or a
JSON file path. Ambiguous shortcuts list the choices. `--preset <name>` selects
another build, and additional game options go after `--`, for example
`python scripts/scenario.py genie -- --no-vsync`. The launcher runs the game
from the repository root; paths in those additional options are relative to it.

Settings live in `data/config.json`, with per-user overrides in
`%APPDATA%\GauntletDarkLegacy\settings.json` or
`~/.config/GauntletDarkLegacy/settings.json`; every string the player sees
comes from `data/text/<language>.json`. Saved characters are kept in a `saves`
folder beside the executable unless `save.directory` in the settings names
another place.

Press **Start** on a controller, or **Escape** on the keyboard during play,
to pause. Save Character and Load Character operate on the player who opened the
menu; slots used by another joined player are protected. Overwrites and loads
require confirmation. Loading restores that character's progress and returns the
party to the tower; it is not a mid-level save state. Once a character has a slot,
travel, returning to the title and shutdown save it automatically.

The title and pause **Options** menus share persisted audio levels, difficulty
and compass visibility. Difficulty changes apply when the next level opens.
The Controls menu remains unimplemented; gameplay bindings can be edited in the
settings file. Failed settings writes show an error. Character/settings files
are replaced only after a complete
temporary file has been written; this is not a power-loss durability guarantee.

## Tests

Run commands from the repository root. Use `python3` instead of `python` on
systems where that is the Python 3 command. The Python helpers select the
native build tools, including the MSVC developer environment on Windows.

### Automated tests

```sh
python -m unittest discover -s tests/scripts -v   # Python helper regression tests
python scripts/build.py --test                   # build Debug, then run non-GPU C++ tests
python scripts/build.py windows-ninja-release --test  # use a specific build instead
```

The first command tests the scripts themselves. `build.py --test` builds the
C++ project and invokes CTest with `-LE gpu`; it does not run the Python tests.
Arguments after `build.py --` are game arguments, not test filters.

C++ tests use Catch2 and mirror `src/` under `tests/`:

| Tier | Needs | Execution |
| --- | --- | --- |
| Asset-free tests | built test executable | run locally and in hosted CI |
| `[assets]` / `[unpacked]` | original disc files / converted assets | included in the non-GPU run, but skip when their required data is absent |
| `[gpu]` (CTest label `gpu`) | Vulkan device and display; some cases also need game data | opt-in locally; Linux CI attempts them with software Vulkan under Xvfb, as a best-effort step |

After building, run or list CTest entries without rebuilding through the
developer-environment helper (replace the preset for your platform/build):

```sh
python scripts/devenv.py -- ctest --preset windows-ninja-release -N
python scripts/devenv.py -- ctest --preset windows-ninja-release -LE gpu -R "Stop Time"
python scripts/devenv.py -- ctest --preset windows-ninja-release -L gpu
```

For Catch2 tag filters or case-name wildcards, invoke the built test executable
through the same helper. These examples use Windows; on Linux use the matching
build directory and omit `.exe`:

```sh
python scripts/devenv.py -- build/windows-ninja-release/bin/tests.exe "[stop-time]"
python scripts/devenv.py -- build/windows-ninja-release/bin/tests.exe "[game][world]~[gpu]"
python scripts/devenv.py -- build/windows-ninja-release/bin/tests.exe --list-tests
```

Adjacent tags mean AND; comma-separated filters mean OR. Rebuild after editing
source: these direct CTest/Catch2 commands run the existing executable.

Hosted CI runs the Python tests and builds/tests C++ on Windows and Linux,
without game assets. A green hosted run does not validate skipped asset tests.
The workflow's optional `game_data` input enables a prepared self-hosted runner
with the original and unpacked files; see [.github/workflows/ci.yml](.github/workflows/ci.yml).

### Scenarios and smoke checks

```sh
python scripts/scenario.py --list
python scripts/scenario.py after-level-shop --build  # build Release and play the tally/shop
python scripts/scenario.py stop-time --build         # exercise an item in a level
python scripts/scenario.py stop-time --frames 120    # bounded run of the existing build
python scripts/build.py --run -- --frames 120        # build and check ordinary startup/shutdown
```

Scenarios require the relevant game data to be extracted and unpacked. They
set up a party and location for interactive playtesting; launching one does not
automatically control the player or assert that combat, audio or visuals match.
A bounded `--frames` run checks startup/runtime/shutdown, not behavioral parity.
Use automated tests that drive actions and assert outcomes for regressions, and
interactive scenarios for visual/audio comparison. Shipped scenarios use unsaved
characters; custom scenarios can name a save slot, so check them before running.

For a focused change, rebuild and run the relevant tests, scoped code-quality
checks below, and a smoke check when runtime code changes. Reserve full-suite
runs for broader changes; missing-data skips are not passes for that behavior.

## Code quality

`.clang-format` is applied on save; `.clang-tidy` holds the lint rules that
clangd shows live. `python scripts/lint.py [path]` runs them over a file, a
folder or the tree, and `python scripts/clangd-check.py [path]` prints every
diagnostic the editor would show. CI builds and tests on Windows and Linux and
lints on Linux. The full lint roster is split into four disjoint concurrent
jobs; every job must pass the aggregate `Lint (clang-tidy)` check. Lint jobs
configure dependencies and build only the `compile_commands` target, not the
game. That configuration disables CMake module scanning (no C++ modules are
used) so lint does not need build-generated module mapper files; normal build
settings are unchanged. To reproduce one partition after a local build, use
`python scripts/lint.py --shard-index 0 --shard-count 4` (indices 0 through 3);
omit those options for the full scan. `--jobs N` limits concurrent processes,
and completed-file counts show progress without changing the checks performed.

## Layout

```
src/engine/   the engine library (namespace gdl): core, math, io, platform, render, codec, audio, assets, ui, world, app
src/formats/  readers for the console asset formats (namespace gdl::formats), used by the tools and tests
src/game/     the game (namespace gdl::game): config, players, menu, screens, world, app, main.cpp
tools/        vqdump (movie to PNG and WAV) and gdlunpack (console assets to standard files)
tests/        Catch2 tests mirroring src/, plus tests/scenarios/ for launching the game onto a moment
shaders/      GLSL sources, compiled at build time to bin/shaders/*.spv
data/         shipped settings defaults and text tables
scripts/      Python helpers: setup, devenv, configure, build, scenario, lint, clangd-check
```

[AGENTS.md](AGENTS.md) has the working rules for contributors and coding agents.

## License

Not yet decided. The code is original; the game data belongs to its owners.
