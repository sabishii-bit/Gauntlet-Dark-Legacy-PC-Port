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

`--run` builds and launches the release build; the Debug build the tests use
runs the tower at well under its frame rate.

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
runs it for you; `gdlunpack <assets> <out> --only <folder>` converts one
folder.

```
gauntlet [--assets <dir>] [--unpacked <dir>] [--data <dir>] [--title] [--movie <name>]
         [--scenario <file>] [--frames <n>] [--no-vsync] [--validation | --no-validation]
```

`--title` skips the intro movies, `--movie <name>` plays one movie and quits,
`--frames <n>` quits after that many frames, and `--scenario <file>` opens the
tower straight onto a described party and place (see `tests/scenarios/`).
Settings live in `data/config.json`, with per-user overrides in
`%APPDATA%\GauntletDarkLegacy\settings.json` or
`~/.config/GauntletDarkLegacy/settings.json`; every string the player sees
comes from `data/text/<language>.json`.

## Tests

The tests are Catch2, under `tests/` in the same shape as `src/`, and come in
three tiers:

| Tier | Needs | How it is run |
| --- | --- | --- |
| Unit tests | nothing | always; this is what CI runs |
| `[assets]` and `[unpacked]` tests | the game data, and the unpacked files | run with the rest; they skip themselves when the data is absent |
| `gpu` label | a Vulkan device and a display | `ctest -L gpu`; excluded from `--test` and CI |

```
python scripts/build.py --test                    # build, then the whole suite (ctest -LE gpu)
ctest --preset windows-ninja-debug                # the suite through CTest, GPU tests included
ctest --preset windows-ninja-debug -R unit.       # only the entries whose name matches
build/windows-ninja-debug/bin/tests               # the binary itself: everything
build/windows-ninja-debug/bin/tests "[collision]" # one tag, or several: "[game][world]"
build/windows-ninja-debug/bin/tests "a cylinder is pushed out of walls*"   # one case by name
build/windows-ninja-debug/bin/tests --list-tests  # what there is
```

Anything in the game is verified by a test against the unpacked data or by
launching a scenario (`gauntlet --scenario tests/scenarios/tower-crystals.json`)
and looking; CI cannot see the game data, so that tier runs on a machine that
has it (`.github/workflows/ci.yml` has a `game_data` switch for a self-hosted
runner).

## Code quality

`.clang-format` is applied on save; `.clang-tidy` holds the lint rules that
clangd shows live. `python scripts/lint.py [path]` runs them over a file, a
folder or the tree, and `python scripts/clangd-check.py [path]` prints every
diagnostic the editor would show. CI builds and tests on Windows and Linux and
lints on Linux.

## Layout

```
src/engine/   the engine library (namespace gdl): core, math, io, platform, render, codec, audio, assets, ui, world, app
src/formats/  readers for the console asset formats (namespace gdl::formats), used by the tools and tests
src/game/     the game (namespace gdl::game): config, players, menu, screens, world, app, main.cpp
tools/        vqdump (movie to PNG and WAV) and gdlunpack (console assets to standard files)
tests/        Catch2 tests mirroring src/, plus tests/scenarios/ for launching the game onto a moment
shaders/      GLSL sources, compiled at build time to bin/shaders/*.spv
data/         shipped settings defaults and text tables
scripts/      Python helpers: setup, devenv, configure, build, lint, clangd-check
```

[AGENTS.md](AGENTS.md) has the working rules for contributors and coding agents.

## License

Not yet decided. The code is original; the game data belongs to its owners.
