# Gauntlet Dark Legacy

A from-scratch **C++26 / Vulkan** reconstruction of the engine behind
*Gauntlet Dark Legacy* (GameCube release, `GUNE5D`), written to be readable,
modular and easy to extend. It reads the original game data; no assets are
included, you need your own copy of the disc.

**Status:** the build opens a window, brings up a Vulkan 1.3 device and plays
the game's intro movies (the Midway logo, the opening and the title movie) with
sound, looping through the attract sequence. No game screens are drawn yet.

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

### Windows

From any PowerShell:

```powershell
.\scripts\configure.ps1          # Ninja + MSVC, Debug
.\scripts\build.ps1 -Test        # build, then run the unit tests
.\scripts\build.ps1 -Run         # build, then launch build\windows-ninja-debug\bin\gauntlet.exe
```

The scripts enter a Visual Studio x64 developer environment for you. By hand,
from an *x64 Native Tools* prompt:

```powershell
cmake --preset windows-ninja-debug
cmake --build --preset windows-ninja-debug
ctest --preset windows-ninja-debug
```

`cmake --preset windows-vs2022` works from any shell and writes
`build\windows-vs2022\GauntletDarkLegacy.sln`.

### Linux

```bash
./scripts/configure.sh                        # linux-ninja-debug (system default compiler)
./scripts/build.sh linux-ninja-debug --test
./scripts/build.sh linux-ninja-debug --run
```

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

```
gauntlet [--assets <dir>] [--movie <name>] [--no-vsync] [--validation | --no-validation] [--frames <n>]
```

`--assets` defaults to `assets/GUNE5D/Gauntlet` (baked in at configure time as
`GDL_ASSET_DIR`). `--movie opening` plays one movie from `VQMOVIES` and quits.
`--frames <n>` quits after `n` frames, handy for smoke tests. Escape quits;
Enter, Space, Start or A skips the current movie.

`vqdump <movie.avi> <out-dir> [--every n] [--max-frames n]` decodes a movie to
PNG frames and a WAV file without running the game, for checking the codec.

## Tests

Catch2 tests live in `tests/`, mirroring `src/`. `ctest --preset <preset>`
runs the unit tests; the GPU integration tests (a real window and device for a
few frames) are registered under the `gpu` label, so `ctest -LE gpu` skips them
on headless machines and `ctest -L gpu` runs only them. Tests tagged `[assets]`
read the game data and skip themselves when it is not installed. The test
binary itself accepts Catch2 tag filters: `build/<preset>/bin/tests "[math]"`.

## Code quality

* `.clang-format` is applied on save by the editor; run `clang-format -i` on
  anything edited elsewhere.
* `.clang-tidy` holds the lint rules. clangd applies them live in the editor;
  `python scripts/lint.py` runs them over the whole tree.
* `python scripts/clangd-check.py` prints every diagnostic the editor would
  show, for every file, and is the definition of "clean".

## Layout

```
src/engine/       reusable engine library (namespace gdl): core, math, io, platform, render, codec, audio, app
src/game/         the Gauntlet game built on it (namespace gdl::game) and the executable
tools/            command-line tools built on the engine (vqdump)
tests/            Catch2 tests, one file per source module, same tree shape as src/
shaders/          GLSL sources, compiled at build time to bin/shaders/*.spv
assets/           game data (ignored by git)
cmake/            CMake helper modules
scripts/          configure / build / lint helpers
.vscode/          shared editor settings (clangd, CMake Tools, debugging)
```

Headers live next to their sources: a class `Foo` in engine module `render` is
`src/engine/render/Foo.h` and `Foo.cpp`, included as `"engine/render/Foo.h"`;
game code is included as `"game/Foo.h"`. `gdl` is the project namespace
(*Gauntlet Dark Legacy*); folders are named by role.

[AGENTS.md](AGENTS.md) has the working rules for contributors and coding agents.

## License

Not yet decided. The code is original; the game data belongs to its owners.
