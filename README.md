# Gauntlet Dark Legacy

A reconstruction of *Gauntlet Dark Legacy* (GameCube release, `GUNE5D`) in
C++26 on Vulkan, written to be readable, modular and easy to extend. It reads
the original game data; no assets are included, you need your own copy of the
disc.

## Controller controls

The shipped pad configuration preserves the **GameCube default button labels** on an
XInput controller, rather than adopting the original Xbox layout. L/R become LT/RT.
The left stick moves; the D-pad selects inventory items.

| Input | Action |
| --- | --- |
| A | Quick attack |
| Y | Slow attack |
| B | Defend |
| Hold B, press A | Turbo attack (meter-dependent) |
| Tap X | Use magic |
| Hold X | Throw magic |
| Double-tap X | Magic shield |
| A + X | Throw magic directly |
| B + X | Magic shield directly |
| LT | Charge |
| RT | Strafe |
| Start | Start/menu input (in-game pause menu not implemented yet) |
| A / Y in menus | Accept / back |

These map to existing gameplay actions; they do not implement the still-missing
two-player partner-combo system (GameCube Z) or the complete linked melee-combo
chains. Bumpers remain free rather than being assigned a nonfunctional partner move.

Target assist is built-in gameplay, not a settings toggle. Normal and strong
throws select a live damageable target in the forward cone, reject intervening
walls, and aim at its height. Stationary attacks face that target; movement and
strafe retain their heading. Projectiles do not home after release, and authored
turbo volleys retain their spread patterns.

Bindings live in `data/config.json`, under `controls.play.keyboard` and
`controls.play.pad`. Per-user overrides live at
`%APPDATA%/GauntletDarkLegacy/settings.json` on Windows (the platform configuration
directory on other systems). No keybinding UI is implemented yet. Each array lists
**alternative** buttons, not a chord; an empty array unbinds the direct action.
`LeftTrigger` and `RightTrigger` are bindable like other buttons (press at 50%,
release below 40% to reject trigger noise).

Chords combine **actions**, on the same device: `turbo` + a new `attack` press,
`attack` + `usePotion`, and `turbo` + `usePotion`. Rebinding those actions moves
their chords automatically. Magic chords consume their constituent requests;
shield takes priority over throw when all three buttons are held. Keyboard
E/Q/C remain immediate use/throw/shield shortcuts; keyboard action chords also work.
For example, this override moves quick attack and its chords to RB:

```json
{"controls":{"play":{"pad":{"attack":["RightBumper"]}}}}
```

`controls.play.padMagicGestures` enables tap/hold/double-tap interpretation of the
configured pad `usePotion` action; disable it for immediate casting with separately
bound `throwPotion`/`shieldPotion` buttons. `actionChords` can also be disabled.
`magicHoldSeconds` and `magicDoubleTapSeconds` are configurable recognition windows
in `(0, 2]` seconds, defaulting to 0.25. A tap waits for the double-tap window after
release, so it cannot spend a potion before the second tap selects a shield. These
timings are port input policy, not verified retail constants. Gesture state resets
on disconnect, reassignment and level entry. A future pause/rebinding UI should reset
the reader when changing input context, not clear it whenever a menu-bound button is
pressed during play (that button may also be a rebound gameplay action).

Control reference: [GameCube manual](https://manuals.plus/m/7e5e4e902caf7e9c695786cbd949d4b9e07d712eb95eaf7b57bdb3eca06beee4_optim.pdf)
and [control comparison](https://strategywiki.org/wiki/Gauntlet_Dark_Legacy/Controls).

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
folder.

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

The title and pause **Options** menus share persisted audio levels, difficulty,
compass visibility and gameplay bindings. Left/right adjusts values; confirm
opens a page or captures a binding. Difficulty changes apply when the next level
opens. The compass uses world north (+Z), rotating with the view. Controls lets
you choose keyboard/controller and an action, replace or clear its binding, or
restore defaults. Action combinations follow the remapped actions. Escape or the
controller's Back button cancels capture; Start remains reserved for pausing.
Changes are saved to the per-user settings file immediately, with a visible error
if persistence fails. Character/settings files are replaced only after a complete
temporary file has been written; this is not a power-loss durability guarantee.

## Tests

### End-level tally and item shop

Successful level exits now show each surviving player's gold, kill and experience
tally, then attributes, shopping, and updated attributes before continuing.
Players shop independently; travel waits for everybody to select Exit and confirm.
Menu directions select items, Confirm buys, Back sells for 75% of the listed price,
and Start moves the cursor to Exit. These use the existing remappable menu bindings
(defaults: arrows/D-pad, Enter or A to buy, Backspace or Y to sell).
Purchases update the character carried into the next stage and its existing save slot.
Fallen players retain their rollback save and do not shop.

For an existing asset installation, export the new catalog and level tally scales:

```powershell
build/windows-ninja-release/bin/gdlunpack.exe assets/GUNE5D/Gauntlet assets/unpacked --only SHPDATA
build/windows-ninja-release/bin/gdlunpack.exe assets/GUNE5D/Gauntlet assets/unpacked --only WDATA
python scripts/scenario.py after-level-shop
```

The scenario uses a disposable, unsaved character with 5,000 gold. The catalog,
icons, scroll artwork and realm shop music come from the installed game data.
The current tally uses readable counters/bars rather than reproducing the original
stacked-pile artwork and its exact animation; this presentation still needs retail
side-by-side tuning.

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
