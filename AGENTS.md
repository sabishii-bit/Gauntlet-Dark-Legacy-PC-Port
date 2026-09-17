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

`data/` is versioned and ships with the game: `config.json` (the settings
defaults) and `text/<language>.json` (every user-facing string by identifier).

## Layout and naming

```
src/engine/<module>/   reusable engine library, namespace gdl   (core, math, io, platform, render, codec, audio, assets, ui, world, app)
src/formats/           console asset format readers, namespace gdl::formats (archives, textures, fonts, text roms)
src/game/<module>/     the Gauntlet game, namespace gdl::game   (config, players, menu, screens, app); main.cpp is the executable
tools/<tool>/          command-line tools (vqdump: movie -> PNG + WAV; gdlunpack: console assets -> PNG + JSON)
tests/                 Catch2 tests, same tree shape as src/ (tests/engine/..., tests/formats/..., tests/game/...)
data/                  shipped settings defaults (config.json) and text tables (text/<language>.json)
shaders/  assets/  cmake/  scripts/  .vscode/
```

* `gdl` (*Gauntlet Dark Legacy*) is the project namespace; folders are named
  by role. Engine code is `gdl::`, game code is `gdl::game::`.
* Headers sit next to their sources. A class `Foo` in engine module `render`
  is `src/engine/render/Foo.h` + `Foo.cpp`, included as
  `"engine/render/Foo.h"`; game files follow the same shape, `"game/menu/Foo.h"`.
* CMake targets: `engine` (static library), `formats` (static library on top
  of the engine; the game never links it), `game` (static library with
  everything but `main.cpp`, so tests can link it), `gauntlet` (executable),
  `tests` (Catch2 executable), `vqdump` and `gdlunpack` (tools).
* Layering, lowest first: `core`, `math`, `io`, `platform`, `render`, `codec`,
  `audio`, `assets`, `ui`, `world`, `app`, then the game. A module only includes
  modules below it.
* Game modules, lowest first: `config` (settings), `players` (the class
  table and stats, the experience curve, character saves, movement input and
  `PlayerActor`, a character standing in a level), `world` (the levels the
  game plays in, `TowerWorld`, and their cameras), `menu` (input mapping,
  menus, name entry and their effects), `screens` (whole screens such as the
  title, movie, player select and tower screens, the status boxes, plus
  `GameContext`, what a screen receives), `app` (the `Gauntlet` driver, the
  command line and the attract flow). The same rule applies: a module only
  includes those below it, and `main.cpp` uses `app`.
* Saved characters are JSON files written by `players/CharacterSave` into the
  directory the settings name (`GameConfig::saveDirectory()`), one per slot;
  the format carries a version so it can grow. Only the select screen writes
  them. Per-class tuning comes from `assets/unpacked/pdata/<CLASS>.json`
  through `players/ClassData`; class and colour codes (`WAR`, `RED`) are asset
  names and live in code, everything a player reads comes from the text
  tables.
* The player select screen is four `screens/SelectLane` state machines under
  one `screens/PlayerSelectScene`; each lane reads one player's devices through
  `MenuInputSource::forPlayer`, with `text` set while the lane takes a name so
  the letter, digit, space and Backspace keys type into it instead of
  steering; Backspace on an empty name does nothing, and the `escape`
  binding (Escape) leaves the name for the menu, which is why the app's
  global Escape quit stands down while a lane is typing. It ends when every
  joined player is locked in and nothing is animating, or when the last
  player backs out.
* The tower (`screens/TowerScene`) takes the locked-in lanes as `PartyMember`s
  into the shared `world/TowerWorld` that `GameContext::tower` carries (the
  select screen looks into the same one). `engine/world/WorldCollision` holds
  a level's `collision.json` triangles, already in world space, on a ground
  grid: `floorAt` probes down for the highest floor, `resolveWalls` pushes a
  cylinder out of walls. `world/TowerCamera` follows the party from the
  `triggerCamera` markers' angles (a marker's yaw points back the way the
  party came, so both the camera and the start heading take it minus pi).
  Glowing objects (`WorldObject::kAdditive`) draw last with
  `BlendMode::Additive`, which never writes depth.
* Draw state: `RenderDevice::draw` takes a `DrawState` (blend, lightmap,
  coordinate offset, alpha test, back-face culling, depth write); the Vulkan
  device sets cull mode and depth write dynamically and pushes the offset and
  alpha test beside the transform. Meshes are wound the way the console
  culls them: `formats/GeometryStream` undoes each strip's alternation and
  turns a strip whose first vertex has the other parity of the packet's
  `flat` flag, which leaves front faces counter-clockwise on screen, so
  world and figure draws cull back faces. Translucent surfaces use
  `DrawState::kTranslucentAlphaTest`, dropping texels under 3/255 so their
  clear parts write no depth.
* `world/WorldScene` keeps still geometry in per-texture batches but stands
  animated objects (and anything under one) and objects flagged for sorting
  as units placed and lit every frame; sorted units draw farthest from the
  eye first with the flag biases, particle markers are never drawn, external
  textures come by name from lender sets (the level's item archive), and
  `setTextureFrame`/`setTextureOffset` swap or slide a texture slot.
  `world/WorldAnimator` plays a layout's keyframed objects thirty frames a
  second (loop, once, backwards once, or off with both flags) through
  `TreePose::sample`; `world/TextureAnimator` steps the archive's texture
  animations once a game frame (frames from the set after the source, or by
  name across the lenders; scrolls by a fraction of the cycle). Both come from
  `formats/AnimationTree` (`textureAnimations`) and `formats/WorldFile`
  (`animations`, `particles`, `itemInfos`, `itemInstances`), with the key
  reader shared in `formats/KeyframeTrack`.
* Particles: `world/ParticleSystem` is the original's emitter. A level's
  template (a letter, a built-in preset to start from, and the fields it
  fills) resolves through `ParticleDescriptor::fromTemplate` over
  `particlePresets()` (the console's eight, kept as data) into frames and
  units: emit for its frames at a rate running between its keys, fade, then
  stop or start over; each particle leaves a random point of the volume along
  a direction in the cone at the speed, is lifted or dropped by gravity, and
  follows colour, alpha and width envelopes over its life and fade.
  `world/ParticleField` starts one emitter per marker (`PSYS` then the
  letter in the marker's name) and draws them as camera-facing squares after
  the geometry, added onto the frame when the template says so. The
  descriptor pool for texture descriptor sets grows on demand.
* Items: `assets/WorldLayout` also carries the level's item kinds and
  instances; `game/world/PlacedItems` stands every powerup instance's figure
  (found by name in the level's item archive, then `POWERUPS`) a tenth of a
  unit above the floor, shown while the party meets the instance's minimum
  (exactly that many when the minimum is marked past ten). Tree nodes flagged
  additive or no-depth-write (`TreeNodeInfo::additive`, `writesDepth`) draw
  that way. `PlacedItems::collect` takes what a `Collector` stands on (the two
  radii sideways, the item's height and a little up or down) and starts the
  crystal's burst: the `GETGEM<colour>` tree of `POWERUPS` has only particle
  nodes (`TreeNodeInfo::particle` names one of the archive's own templates,
  `direction` the way it emits), run in a `ParticleField` for the sequence's
  length. A crystal's value indexes `kCrystalRealms` for the realm it counts
  towards; `TowerScene::collectItems` gives every party member one, up to what
  the gate wants, and plays the common bank's pickup chime. Crystal counts live
  in `ClassProgress::crystals` (one per realm, in the save's JSON). Every item
  plays its first sequence on a loop (`Item::player`, `pose`: the crystals
  turn), and a burst plays the GETGEM tree's sequence with its emitters riding
  their nodes (`Effect::pose`, `ParticleField::setNode`). `hideCrystals` and
  `reveal` fade the crystals in for Sumner's welcome the original's way: from
  the world's origin outward at 15 units a second, starting 1.75 seconds'
  worth out, 8/255 of alpha a frame once reached (`TreeModel::draw` takes an
  alpha).
* Pickup HUD: `screens/PickupHud` keeps the cards (the `S3` strip with a
  STATIC card such as `CRYSTAL` under it, rising a pixel a tick from under the
  screen to the bar over the taker's box, holding ninety ticks, falling away)
  and the three-second counts (a `SM_CRYSTAL_*` icon and "have/need") that
  `StatusBoxPainter::drawCard` and `drawCount` paint; `TowerScene::collectItems`
  feeds it and `render` draws it over the boxes, under the scroll.
* Triggers: `game/world/LevelTriggers` reads the layout's trigger items (type
  5): the target object from the instance's first parameter word, the trigger
  flags from its second (0x40 = wants the realm the id names, the kind's
  low byte drives the target: 0x10 fades), the radius from byte 4 (half units,
  0xFF a hair), the id and next id from bytes 6 and 7. Targets that the layout
  animates are held at their first frame by `WorldAnimator::hold` until
  `fire` plays them once; chains follow next ids among triggers wanting no
  crystals; `openMet` opens at level start whatever the party qualifies for;
  fading targets lose their collision (`WorldCollision::setSolid`) and thin
  out through `WorldScene::setObjectAlpha`. `TowerScene` passes the party as
  `TriggerVisitor`s each frame.
* Collision follows animation: level files keep the triangles of any object
  flagged to move (`WorldObject::kAnimated`, set on the animated and on the
  force fields that only fade) in the object's own space, the rest in world
  space. `WorldCollision::setMovingObjects` takes those aside and
  `setObjectTransform` places them from the scene's `worldTransform` after
  every animator step (`TowerWorld::syncCollision`).
* Camera markers are the `cameraGame` locators (the original's marker table
  keeps only those enabled for the follow camera; trigger cameras serve the
  cuts). A marker's `delay` byte is a fixed camera distance when not zero.
* Additive geometry (`WorldObject::kAdditive`: glows, flames, the force
  fields) is drawn unlit (`WorldScene::kUnlit`), as the original never lights
  it; everything else takes `WorldLighting`. The streams' prelit vertex
  colours are still skipped by the geometry decoder.
* Facing: an object's flags carry a facing mode in their top nibble
  (`CameraFrame::facingOf`): mode 4 takes the camera's whole frame (a proper
  rotation with z back at the camera), the others turn about the vertical to
  face it. `TreeModel::draw` and `WorldScene::draw` take a `CameraFrame`
  (`CameraFrame::of(camera)`) and apply it per node or unit.
* Scenarios: `app/Scenario` reads a JSON start (party members by class and
  colour code, level and crystals; position, yaw, welcome) into
  `PartyMember`s and `TowerOptions`, and `--scenario <file>` opens the tower
  onto it. The scenario files are test data and live in `tests/scenarios/`.
  Use them, with a capture script, to verify a moment in play instead of
  driving through the title and select screens.
* Lighting: `engine/world/WorldLighting` is the original's vertex shade, a grey
  ambient plus one directional light where a surface faces it, clamped per
  channel; `WorldLighting::forLevel` takes a level's record (the light
  direction is the way the light travels, so it is negated). Levels carry
  baked lightmaps: `formats/GeometryStream` reads the second coordinate pair
  of the four-value texcoord format into `MeshVertex::lightmapUv` (texels of
  the lightmap; `WorldScene` divides by the lightmap's size) and the
  sub-object's lightmap texture into `MeshPart::lightmap`; the OBJ writer and
  reader carry them as `vl` lines and `_lm<index>` material suffixes. The
  render device takes an optional lightmap texture per draw, sampled with
  `ImmediateVertex::uv2`, whose alpha scales the colour (the console's second
  texture stage); the Vulkan pipeline binds it as descriptor set 1, white when
  a draw has none. `formats/WorldDataWad` reads `WDATA/*.WAD` (levels, camera
  and audio records) through the shared `formats/WadDirectory`; gdlunpack
  writes `wdata/<REALM>.json`, `assets/WorldData` loads it, and
  `game/world/TowerWorld` takes its light, camera range and sound names from
  it instead of constants.
* Sumner's welcome: `assets/MessageTable` reads an unpacked text rom
  (`text/scroll_e.json`: fonts, messages with pages). `game/menu/ScrollBox`
  is the original's controller message box: the scroll art sized to the page
  (text plus 96, no narrower than the prompt plus 32, at most 512, centred on
  (256, 160)), the text centred 32 below its top with 4 between lines, the
  glowing prompt and button icon 8 below the text, a 15-tick hold before a
  page takes any joined player's button, and the fire scroll after the last
  page; the prompt is a string-table entry. `game/world/SumnerFigure` is the
  GWIZ tree of `ITEMS/LEVELL` at the event marker whose parameter is 0,
  cycling READY, READING and THINKING as the original's index does (advance
  whenever a sequence ends or changes, wrap past 2) and cutting to GESTRIGHT
  (index 6) on request. `TowerScene` runs the welcome for a party with no
  class experience: the scroll holds everything still, then the crystal
  trigger camera (marker 198, its raw yaw and pitch) shows for 300 ticks with
  the controls off; `viewCamera()` is what the scene renders through.
* Streams: `engine/audio/StreamSource` is audio decoded a piece at a time;
  `engine/audio/AdsStream` reads one of the game's `STREAMS/*.ads` files through
  the ADS codec and rewinds to loop. `SoundPlayer::playStream` plays a source
  as a voice like any sequence (fed ahead of the mixer, looping by rewinding,
  under the category volumes). Screens reach the shipped files through
  `GameContext::assets`; the tower plays the stream its realm's audio record
  names at the level's music volume, and footsteps come from the animator's
  `footfall()` (the foot that came down as a walk or run half cycle ended,
  as the original flags them) through the common bank's step sounds.
* Animation: `formats/AnimationTree` decodes every sequence's keys (plain or
  delta-table compressed) into `NodeTrack`s that `assets/AnimationSet` loads
  as `TrackInfo`; `engine/world/AnimationPlayer` steps one sequence on the game
  clock (900 over the rate frames a second, whole frames, loop or hold, a
  transition holding the first frame); `engine/world/TreePose` samples every
  node's keys (angles interpolate only across steps under a right angle) and
  composes the node matrices in the original's rotation orders, blending
  between poses for transitions; `TreeModel::draw` takes those matrices.
  `game/players/PlayerAnimator` is the player's action logic: which sequence
  the stick asks for, the four cut-in rules, the entrance, the stance loop,
  the two fidgets and their tick timers, and the alternating walk and run
  halves. A costume's figure follows the class tree's tracks by node name
  (`PLAYERS/<CLASS>/ANIM`), so one class file animates every costume.
* Vulkan appears only under `src/engine/render/vulkan/`, GLFW only
  under `src/engine/platform/`, miniaudio only in
  `src/engine/audio/AudioDevice.cpp`, stb_image only in `src/engine/assets/`,
  nlohmann-json only in `.cpp` files under `src/engine/assets/`.
* Nothing the player sees or tunes is hard-coded. Sizes, rates, volumes and
  bindings are fields of `game/config/GameConfig`, defaulted in code and in
  `data/config.json` (the two must agree; a test checks) and overridden by the
  user's settings file. Every user-facing string is looked up in
  `assets/StringTable` by identifier (`data/text/en.json`); code never holds
  a literal the player reads. A screen receives all of this through
  `game/screens/GameContext`.
* 2D screens draw through `ui/Canvas` in the configured virtual space (the
  original's 512x384) and `ui/TextPainter` for bitmap text; `ui/ModelSprite`
  draws an animation tree's meshes as a lit 3D object on that canvas, sized
  from the configured camera. Screen logic runs on the configured tick count
  (60 Hz; gameplay was tuned for two ticks per frame) through
  `step(ticks, input)` so tests can drive it without a clock.
* Textures flagged clamp in their manifest are sampled with edge clamping, so
  tiles that meet edge to edge show no seam.
* Levels: `assets/WorldLayout` reads a level's `world.json` (placed objects,
  their parent links and marker points); `world/WorldScene` gathers the placed
  meshes per texture, lit per vertex, and `world/WorldCamera` places a camera
  with the original's pitch/yaw/roll convention, projecting depth into
  [0, 0.45] so the 2D layers at 0.5 and 0.75 always draw on top. Levels are
  unpacked only with `gdlunpack --levels` (or `--only <level>`), about 20 MB
  each.
* Sounds are `assets/SoundSet` entries (a bank's `sounds.json`) played through
  `audio/SoundPlayer` in a `SoundCategory` (effects or music, scaled by the
  audio settings), which feeds sample sequences and loops into mixer
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
  `tests/game/screens/MovieSceneTests.cpp`); extend those rather than skipping
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
