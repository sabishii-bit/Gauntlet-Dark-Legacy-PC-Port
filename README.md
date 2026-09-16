# Gauntlet Dark Legacy

A from-scratch **C++26 / Vulkan** reconstruction of the engine behind
*Gauntlet Dark Legacy* (GameCube release, `GUNE5D`), written to be readable,
modular and easy to extend. It reads the original game data; no assets are
included, you need your own copy of the disc.

**Status:** the build opens a window, brings up a Vulkan 1.3 device, plays
the game's intro movies with sound and then shows the title screen: the logo,
its animated glow, "Press Start", the title music, and the Start / Options
menus drawn with the game's own fonts, scroll art, menu sounds and the 3D
arrow cursor. Choosing Start returns to the attract loop (player select is
next); the Options entries are listed but not yet wired up. The title screen
needs the assets unpacked once with `gdlunpack` (see Run).

## Requirements

Common to every platform:

| Requirement | Notes |
| --- | --- |
| CMake ≥ 3.30 and Ninja | 3.30 is the first release that knows C++26. See the platform notes for PATH pitfalls. |
| vcpkg | Set `VCPKG_ROOT` to your checkout; the scripts fall back to `C:\vcpkg` on Windows and `~/vcpkg` on Linux |
| Vulkan 1.3 capable GPU driver | Any current NVIDIA / AMD / Intel driver (Mesa 22+ on Linux) |
| LLVM tools (optional) | `clangd`, `clang-format`, `clang-tidy` for the editor integration and the lint script |
| LunarG Vulkan SDK (optional) | Provides the validation layer (auto-enabled in Debug builds when present) and `glslc` |
| Game assets | Extract your disc into `assets/GUNE5D`, see [assets/README.md](assets/README.md) |

Dependencies (GLFW, GLM, volk, Vulkan Memory Allocator, glslang, Catch2) are
fetched and built by vcpkg on the first configure. That takes a few minutes
once; later configures hit the binary cache.

### Windows

| Requirement | Notes |
| --- | --- |
| Visual Studio 2022 (IDE or Build Tools) | "Desktop development with C++" workload: MSVC 14.4x, Windows SDK, and ideally the "C++ CMake tools for Windows" component (bundles CMake 3.31 and Ninja). MSVC has no `/std:c++26` switch yet, so the build uses `/std:c++latest`, its C++26 preview. |
| A **native Windows** CMake | An MSYS2/Cygwin CMake on `PATH` cannot drive MSVC and breaks vcpkg |
| A real `ninja.exe` | Wrapper scripts such as pyenv-win's `ninja.bat` shim cannot be launched by CMake. The scripts prefer the Visual Studio copies. |

### Linux

GCC 14+ or Clang 17+ (for `-std=c++26`), CMake 3.30+, and the system
libraries vcpkg's GLFW build needs. On Debian/Ubuntu:

```bash
sudo apt install build-essential g++-14 cmake ninja-build pkg-config curl zip unzip tar \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libxkbcommon-dev libwayland-dev libgl-dev libvulkan1 mesa-vulkan-drivers
# optional: validation layer, glslc, LLVM editor tooling
sudo apt install vulkan-validationlayers glslang-tools clangd clang-format clang-tidy
```

If the distribution's CMake is older than 3.30, use Kitware's apt repository
or `pip install cmake`.

## Build

The helper scripts are Python (3.9 or newer) and behave the same on Windows
and Linux:

```
python scripts/configure.py                # configure the platform's Debug preset
python scripts/build.py --test             # build, then run the unit tests
python scripts/build.py --unpack           # build, then unpack the console assets (see Run)
python scripts/build.py --run -- --title   # build, then launch the game with arguments
```

On Windows the scripts enter an x64 MSVC developer environment themselves and
prefer the CMake and Ninja bundled with Visual Studio; `python
scripts/devenv.py --shell` opens an interactive shell in that environment for
running `cmake --preset ...` by hand, and `python scripts/devenv.py` prints
which tools it found. On Linux they only fill in `VCPKG_ROOT` from `~/vcpkg`
when it is unset. `python scripts/configure.py --fresh` discards a cache that
picked up the wrong tool.

`cmake --preset windows-vs2022` works from any shell and writes
`build\windows-vs2022\GauntletDarkLegacy.sln`.

### Presets

Presets are filtered by host OS; `cmake --list-presets` shows the applicable ones.

| Configure preset | Build / test preset | Toolchain |
| --- | --- | --- |
| `windows-ninja-debug` | `windows-ninja-debug` | Ninja + MSVC, Debug |
| `windows-ninja-release` | `windows-ninja-release` | Ninja + MSVC, RelWithDebInfo |
| `windows-vs2022` | `windows-vs2022-debug` / `windows-vs2022-release` | Visual Studio 17 2022 solution |
| `linux-ninja-debug` | `linux-ninja-debug` | Ninja + default compiler, Debug |
| `linux-ninja-release` | `linux-ninja-release` | Ninja + default compiler, RelWithDebInfo |
| `linux-clang-debug` | `linux-clang-debug` | Ninja + Clang, Debug |
| `linux-clang-release` | `linux-clang-release` | Ninja + Clang, RelWithDebInfo |

Each preset builds into `build/<configure-preset>/`, with the executables,
the compiled shaders and (on Windows) the dependency DLLs under `bin/`.

## Run

Unpack the console-specific asset files once (PNG images plus JSON manifests
go under `assets/unpacked`, which git ignores):

```
python scripts/build.py --unpack
```

Then run `build/<preset>/bin/gauntlet` (or `python scripts/build.py --run --
<arguments>`):

```
gauntlet [--assets <dir>] [--unpacked <dir>] [--movie <name>] [--title] [--no-vsync]
         [--validation | --no-validation] [--frames <n>]
```

`--assets` defaults to `assets/GUNE5D/Gauntlet` and `--unpacked` to
`assets/unpacked` (both baked in at configure time as `GDL_ASSET_DIR` and
`GDL_UNPACKED_DIR`). `--movie opening` plays one movie from `VQMOVIES` and
quits; `--title` skips the intro movies. `--frames <n>` quits after `n`
frames, handy for smoke tests. Escape quits. During a movie, Enter or Start
jumps to the title screen and Space or A skips to the next attract screen. On
the title screen, Enter or Start opens the menu; arrows or the d-pad move,
Enter, Space or A selects, Backspace or B goes back.

### Tools

`gdlunpack <asset-root> <out-root> [--only <directory>]` converts every
`objects.ngc` / `textures.ngc` archive with its `ANIM.PS2`, the `AUDIO`
sound banks, `FONTS/*.fnt` fonts and `TEXT/*.rom` string tables into
standard files (about 130 MB in total):

```
assets/unpacked/<ARCHIVE>/textures/<index>_<NAME>.png   decoded textures (RGBA PNG)
assets/unpacked/<ARCHIVE>/textures.json                  names, sizes, flags, animation frames
assets/unpacked/<ARCHIVE>/models/<index>_<NAME>.obj      meshes (Wavefront OBJ, one group per texture)
assets/unpacked/<ARCHIVE>/objects.json                   object names, mesh files and sub-object data
assets/unpacked/<ARCHIVE>/animations.json                animation trees: node hierarchy, objects, sequences
assets/unpacked/audio/<BANK>/samples/<index>.wav         decoded samples (16-bit PCM)
assets/unpacked/audio/<BANK>/sounds.json                 named sounds: sample sequences, loops, volumes
assets/unpacked/fonts/<name>.json                        glyph cells of each bitmap font
assets/unpacked/text/<name>.json                         fonts, named messages and message lists
```

Keyframe data of the animation trees is not exported yet; the trees are
static poses.

The game reads only these unpacked files; the console formats are handled by
the `formats` library and this tool.

`vqdump <movie.avi> <out-dir> [--every n] [--max-frames n]` decodes a movie to
PNG frames and a WAV file without running the game, for checking the codec.

## Tests

Catch2 tests live in `tests/`, mirroring `src/`. `ctest --preset <preset>`
runs the unit tests; the GPU integration tests (a real window and device for a
few frames) are registered under the `gpu` label, so `ctest -LE gpu` skips them
on headless machines and `ctest -L gpu` runs only them. Tests tagged `[assets]`
read the game data and tests tagged `[unpacked]` read the `gdlunpack` output;
both skip themselves when that data is absent. The test binary itself accepts
Catch2 tag filters: `build/<preset>/bin/tests "[math]"`.

## Code quality

* `.clang-format` is applied on save by the editor; run `clang-format -i` on
  anything edited elsewhere.
* `.clang-tidy` holds the lint rules. clangd applies them live in the editor;
  `python scripts/lint.py` runs them over the whole tree.
* `python scripts/clangd-check.py` prints every diagnostic the editor would
  show, for every file, and is the definition of "clean".

## Layout

```
src/engine/       reusable engine library (namespace gdl): core, math, io, platform, render, codec, audio, assets, ui, app
src/formats/      readers for the console asset formats (namespace gdl::formats), used by the tools and tests only
src/game/         the Gauntlet game built on it (namespace gdl::game) and the executable
tools/            command-line tools built on the engine (vqdump, gdlunpack)
tests/            Catch2 tests, one file per source module, same tree shape as src/
shaders/          GLSL sources, compiled at build time to bin/shaders/*.spv
assets/           game data (ignored by git)
cmake/            CMake helper modules
scripts/          Python helpers: devenv, configure, build, lint, clangd-check
.vscode/          shared editor settings (clangd, CMake Tools, debugging)
```

Headers live next to their sources: a class `Foo` in engine module `render` is
`src/engine/render/Foo.h` and `Foo.cpp`, included as `"engine/render/Foo.h"`;
game code is included as `"game/Foo.h"`. `gdl` is the project namespace
(*Gauntlet Dark Legacy*); folders are named by role.

[AGENTS.md](AGENTS.md) has the working rules for contributors and coding agents.

## License

Not yet decided. The code is original; the game data belongs to its owners.
