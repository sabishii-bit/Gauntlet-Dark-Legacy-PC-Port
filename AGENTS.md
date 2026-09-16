# Working rules

Read this before changing anything. It applies to people and to coding agents.

## What this repository is

A standalone C++26 / Vulkan reconstruction of the *Gauntlet Dark Legacy*
(GameCube, `GUNE5D`) engine. The goal is code that is readable, modular and
easy to extend, running the original game data. It is not a port of any other
codebase and does not depend on one; the README and the source never mention
other reconstruction or decompilation projects.

There is no `docs/` folder by design. Design rationale goes into commit
messages and pull requests; the code and its tests are the documentation.

Game data is never committed. It lives under `assets/GUNE5D/` (ignored by
git, see `assets/README.md`); the build bakes `assets/GUNE5D/Gauntlet` in as
the default asset directory. The console-specific files (`objects.ngc`,
`textures.ngc`, `ANIM.PS2`, `*.VBK` sound banks, `*.fnt`, `*.rom`) are
converted once by `gdlunpack` into PNG images, OBJ meshes, WAV samples and
JSON manifests under `assets/unpacked/` (also ignored, baked in as
`GDL_UNPACKED_DIR`). The game reads only those standard files; the console
formats are parsed solely in `src/formats/` and `tools/gdlunpack/`. Movies and
audio streams stay in the disc's own containers, decoded by `engine/codec`.

## Layout and naming

```
src/engine/<module>/   reusable engine library, namespace gdl   (core, math, io, platform, render, codec, audio, assets, ui, app)
src/formats/           console asset format readers, namespace gdl::formats (archives, textures, fonts, text roms)
src/game/              the Gauntlet game, namespace gdl::game; main.cpp is the executable
tools/<tool>/          command-line tools (vqdump: movie -> PNG + WAV; gdlunpack: console assets -> PNG + JSON)
tests/                 Catch2 tests, same tree shape as src/ (tests/engine/..., tests/formats/..., tests/game/...)
shaders/  assets/  cmake/  scripts/  .vscode/
```

* `gdl` (*Gauntlet Dark Legacy*) is the project namespace; folders are named
  by role. Engine code is `gdl::`, game code is `gdl::game::`.
* Headers sit next to their sources. A class `Foo` in engine module `render`
  is `src/engine/render/Foo.h` + `Foo.cpp`, included as
  `"engine/render/Foo.h"`. Game files are included as `"game/Foo.h"`.
* CMake targets: `engine` (static library), `formats` (static library on top
  of the engine; the game never links it), `game` (static library with
  everything but `main.cpp`, so tests can link it), `gauntlet` (executable),
  `tests` (Catch2 executable), `vqdump` and `gdlunpack` (tools).
* Layering, lowest first: `core`, `math`, `io`, `platform`, `render`, `codec`,
  `audio`, `assets`, `ui`, `app`, then `game`. A module only includes modules
  below it. Vulkan appears only under `src/engine/render/vulkan/`, GLFW only
  under `src/engine/platform/`, miniaudio only in
  `src/engine/audio/AudioDevice.cpp`, stb_image only in `src/engine/assets/`,
  nlohmann-json only in `.cpp` files under `src/engine/assets/`.
* 2D screens draw through `ui/Canvas` in the original's 512x384 virtual space
  and `ui/TextPainter` for bitmap text; `ui/ModelSprite` draws an animation
  tree's meshes as a lit 3D object on that canvas. Screen logic runs on a
  60 Hz tick count (`step(ticks, input)`) so tests can drive it without a
  clock.
* Sounds are `assets/SoundSet` entries (a bank's `sounds.json`) played through
  `audio/SoundPlayer`, which feeds sample sequences and loops into mixer
  streams; the game calls `SoundPlayer::update()` once per frame. Movie audio
  and sounds stop through `AudioStream::stop()`, which drops what is queued.
* Game data is read through `AssetLocator`, which matches names ignoring case,
  so code uses the original lowercase names and Linux keeps working.
* Texture contents change through `RenderDevice::updateTexture`, called after
  `beginFrame` and before that frame's first draw.
* New modules get their own directory under `src/engine/` and a matching
  directory under `tests/engine/`.

## Language and style

* C++26: `-std=c++26` on GCC 14+ / Clang 17+; MSVC 14.4x has no `/std:c++26`
  switch, so the root `CMakeLists.txt` asks CMake for 23 there, which it emits
  as `/std:c++latest` (the C++26 preview). Use only features all three support.
* `.clang-format` is authoritative; the editor formats on save. Run
  `clang-format -i` over anything edited outside the editor.
* `.clang-tidy` is the linter. clangd applies it live; `python
  scripts/lint.py` runs it over the whole tree. Do not add `NOLINT` without a
  reason in the same comment; prefer restructuring the code.
* Names: types `PascalCase`, functions and variables `camelBack`, members
  `m_`, constants and `constexpr` values `kName`, macros `GDL_*`. Name classes
  by responsibility; no generic suffixes such as `App`, `Manager`, `Helper`.
* Ownership is explicit: `std::unique_ptr` for owned objects, references for
  non-owning access, no raw `new`/`delete`, no mutable globals (function-local
  statics when unavoidable). `std::array` and `std::span` instead of C arrays
  and pointer arithmetic.
* `GDL_VERIFY` / `GDL_FATAL` for programmer errors and unrecoverable setup;
  `log::warn` and a fallback for bad data.
* Comments: a short `/** ... */` doc comment on classes and non-obvious
  functions, `///<` for a member that needs one. Say what the thing is for,
  not how it came to be. No history, no rejected alternatives, no references
  to other codebases.
* Fixed limits are named constants, never magic numbers in expressions.

## Tests

* Every source module has a test file: `src/engine/render/Foo.cpp` is covered
  by `tests/engine/render/FooTests.cpp`. New code ships with its tests.
* Pure logic is unit tested. Drawing code is unit tested against
  `test::FakeRenderDevice` (`tests/FakeRenderDevice.h`), which records draw
  calls. Code that needs a window or GPU is covered by `[gpu]` integration
  tests (`tests/engine/app/ApplicationTests.cpp`,
  `tests/game/MovieSceneTests.cpp`); extend those rather than skipping
  coverage. Tests that read game data carry the `[assets]` tag and skip
  through `test::assetOrSkip` when the data is absent; tests that read the
  `gdlunpack` output carry `[unpacked]` and use `test::unpackedOrSkip`;
  synthetic inputs built with `test::ByteWriter` cover the format parsers
  without either.
* Format decoders get a command-line dumper under `tools/` when one makes the
  output inspectable without the game (see `vqdump`).
* `ctest --preset <preset>` runs everything; `-LE gpu` skips the GPU test on
  headless machines. Tests must pass on Windows and Linux.

## IDE

* `.vscode/` and `.clangd` are versioned so every checkout gets the same
  setup: clangd for diagnostics, formatting and clang-tidy; CMake Tools
  driving the presets; cpptools kept only for its debugger with all of its
  language features disabled (its parser reports phantom errors such as
  `namespace "gdl::log" has no member "error"`). Reload the window after the
  first checkout so the workspace settings apply.
* clangd reads `compile_commands.json` at the repository root; the build
  copies it there, so configure and build once before expecting diagnostics.
* The tree must be free of clangd diagnostics, including clang-tidy findings
  and unused includes. `python scripts/clangd-check.py` (after a build)
  prints exactly what the editor would show for every file. Umbrella includes
  needed for side effects carry `// IWYU pragma: keep`; headers that
  intentionally re-export others use `// IWYU pragma: export`.

## Building and verifying

* Helper scripts are Python only (3.9+), identical on every platform; never
  add shell or PowerShell scripts. `scripts/devenv.py` resolves the
  environment (on Windows: the x64 MSVC developer environment with the
  Visual Studio CMake and Ninja first on PATH; `--shell` opens it
  interactively), `scripts/configure.py [preset] [--fresh]` configures,
  `scripts/build.py [preset] [--test] [--unpack] [--run -- args]` builds and
  then tests, unpacks or launches, `scripts/lint.py` and
  `scripts/clangd-check.py` check the tree. If a stale cache picks up the
  wrong tool, reconfigure with `python scripts/configure.py --fresh`. Keep
  both platforms building.
* Definition of done for any change: `python scripts/build.py --test`
  warning-free and green, `python scripts/lint.py` clean,
  `python scripts/clangd-check.py` clean, and `gauntlet --frames 120` runs to
  a clean shutdown (with `--title` too when the change touches the 2D
  screens; `python scripts/build.py --unpack` first).
* vcpkg pins its baseline in `vcpkg.json`. `VCPKG_ROOT` must be a git
  checkout at or after that commit, bootstrapped so the tool matches its
  scripts; the copy bundled with Visual Studio is too old and the dev-shell
  script steers around it. Bump the baseline rather than patching ports; if a
  port must be patched, add `vcpkg-overlays/` plus `vcpkg-configuration.json`
  and say why in the overlay's README.

## Git

* Do not commit or push unless asked. Never commit anything under `assets/`.
