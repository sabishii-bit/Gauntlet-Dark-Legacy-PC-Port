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
  game plays in, `LevelWorld`, and their cameras), `menu` (input mapping,
  menus, name entry and their effects), `screens` (whole screens such as the
  title, movie, player select and tower screens, the status boxes, plus
  `GameContext`, what a screen receives), `app` (the `Gauntlet` driver, the
  command line and the attract flow). The same rule applies: a module only
  includes those below it, and `main.cpp` uses `app`.
* Saved characters are JSON files written by `players/CharacterSave`, one per
  slot, into `GameConfig::saveDirectory()`: a `saves` folder beside the
  executable (not the per-user folder; that is a holding decision until a
  permanent home is chosen), or `save.directory` from the settings, taken
  beside the executable when it is relative. The format carries a version so
  it can grow. The select screen writes a character when the player saves it;
  after that `players/Party` ties each `PartyMember` to its slot and
  `Gauntlet::keepParty` writes the party in play back (`saveParty`) whenever
  it travels between levels, leaves for the title screen or the game shuts
  down. A character never saved has no slot and is not kept. A scenario
  member may name a `slot` to be kept the same way; the shipped scenarios do
  not, so running one never overwrites a real save. Per-class tuning comes from `assets/unpacked/pdata/<CLASS>.json`
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
* The tower (`screens/PlayScene`) takes the locked-in lanes as `PartyMember`s
  into the shared `world/LevelWorld` that `GameContext::tower` carries (the
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
  towards; `PlayScene::collectItems` gives every party member one, up to what
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
  `StatusBoxPainter::drawCard` and `drawCount` paint; `PlayScene::collectItems`
  feeds it and `render` draws it over the boxes, under the scroll.
* Ambience: `game/world/AmbientSounds` runs the level's sound items (type
  13): a loop named by the instance, found in the level's bank or the
  tower's `TOWAMB`, full within its radius (the first parameter word, a
  float), fading to silence at one and a half radii, panned by the camera's
  right hand (`SoundPlayer::setVolume`/`setPan`, `AudioStream::setPan`), at
  `kPeak` (224/255) times the level's sound volume. `PlayScene` binds it
  once the world is loaded and updates it after the camera each frame.
* Lighting: the level files carry a colour block per vertex (five bits a
  channel, the lighting the level was built with), which the decoder reads
  into `MeshVertex::color` and marks the mesh `prelit`; gdlunpack writes it
  as the OBJ vertex-colour extension and `ObjModel` reads it back. The scene
  shades an object flagged `WorldObject::kPrelit` (0x2, most of a level) by
  those colours, additive parts unlit, and everything else by
  `WorldLighting`. Items and characters are lit by the lights, as the
  original lights them.
* Characters: `PlayScene::costumeDirectory` picks the costume tier of ten
  levels (`PLAYERS/<CLS>/<COL><tier>0`, unpacked with `--tiers`) when it
  exists, else `<COL>`; the weapon is the costume archive's own `WEAP_HOLD`
  (a tiered costume) or `WEAP_<COL>_HD<1|2|3>` (levels 1, 10, 50; the
  untiered costumes) hung from the node whose object ends in the class's
  wrist name (`R_WRIST`, `RIGHTHAN`, `RHEND`). A tree's node named `DUMMY`
  (a costume's marker triangle at the feet) is never drawn (`TreeModel::bind`),
  as the original hides it.
* Levels: `world/LevelWorld` (once `TowerWorld`) loads any level from a
  `LevelRef` (realm data file, level name, `LEVELS/LEVEL<name>`, the realm's
  `ITEMS/LEVEL<letter>`), the tower by default; `screens/PlayScene` (once
  `TowerScene`) plays in it, keeping Sumner, his hints, his welcome and the
  crystal reveal to `LevelWorld::isTower()`. `world/LevelCatalog` reads every
  `wdata/*.json` and resolves an exit's two characters the way the original's
  `FindWave` does: the letter is the last of the realm's folder prefix, the
  digit counts into the realm's own level order (the castle's `a2` is `A6`).
  `world/ExitPortals` stands the `EXIT_PORTAL` figure at every exit item (type
  9; the tag is bytes 4 and 5 of its parameters) and ports its state machine:
  with the whole party on it (radius 3, plus a unit per extra member) it runs
  IDLE, READY, ACTIVE1, ACTIVE2 (held 45 ticks), ACTIVE3 and the party is
  through; with only some it waits at ACTIVE2; left alone it plays out to
  IDLE. `PlayScene::update` then returns `PlayOutcome::Travel` with
  `destination()` and `party()`; `Gauntlet::startLevel` reloads the one
  `LevelWorld` and reopens the scene with `PlayOptions::arrivalWorld` (the
  realm left, which picks the tower's start marker for it). A portal whose
  level is not unpacked goes nowhere from the tower (a warning names it) and
  back to the tower from anywhere else, so nobody is stranded: that fallback
  is ours, not the original's. Only the tower and `G1` (with `ITEMS/LEVELG`)
  are unpacked here: `gdlunpack <assets> <out> --only levelG1`, then
  `--only levelG`. Scenarios take `level` (`tests/scenarios/level-g1.json`).
  Still to come for other levels: which portals a save has opened, and what
  G1 places that nothing handles yet (the rune and scroll pickups).
* The loading screen is `screens/TransitionScreen`: the `TRANSITION_SCREEN`
  texture of the static set drawn 512x320 over the view. Leaving by a portal
  it comes up over two seconds (the original's alpha, 255 * (1 - d / 2)),
  `Gauntlet` draws it covering for a frame before the blocking load, and the
  arriving scene clears it away over half a second (that fade out is ours).
  Without the texture it is plain black.
* Level fixtures (`world/ItemFigure`, `Chests`, `LockedGates`, `Traps`), bound
  from the level's item instances and updated by `PlayScene::updateFixtures`.
  An `ItemFigure` is an item's animated tree placed by `itemPlacement` (yaw
  outermost, shared with `PlacedItems`: instances turned past a quarter come
  as (pi, y, pi), a half turn of yaw) with an `Obstacle`, the record's
  `xSize` by `zSize` box that pushes bodies out and tells who is against it.
  Containers are type 2 (44 trapped, 46 chest, 47 gold; 43 barrels belong to
  `Breakables`), bit 0x10 of `activeType` means locked,
  `params[0]` is the contents record and `params[4]` a count. A record of
  type -1 is a choice list (`ItemInfo::choices`): the pick is
  ((seed >> 5) + item index) % n and each pick moves the seed on by 439.
  Touching a locked chest or gate spends a key (without one the help message
  asks for it); a chest plays ACTIVE then OPEN to `S_CHEST`, and what it held lies in it,
  reached by touching the open chest as the original does (the player cannot
  reach its middle), after which the emptied chest goes. A gold chest pays
  its opener, a trapped one blows up (`EXPCHEST`, a blast of 50). The
  chest tree's `NULL1` node is where the original hangs the contents; it is
  a marker and `TreeModel` never draws it. Gates (type 7) play ACTIV to the
  realm bank's gate sound and stop blocking 30 ticks in. Traps (type 8) rest
  on their first sequence for `activeOff` * 2 ticks (negative: at random from
  half to one and a half of it), play the rest in turn, and while out hurt
  whoever is in their box by the record's value, who is then left alone for
  (ticks left in that sequence + 1) / 30 seconds, as the original leaves
  them. Every trap hit over a point also stuns (`PlayerDeed::Flinch` for
  spikes and blades, which play `HITREACT`, `Reel` for the rest, `STUN1`):
  the victim stands where it was struck until the sequence ends and is not
  set reeling again meanwhile. The class trees' `SPIKEHIT` is named nowhere
  in the original's code and goes unused. Open question: a victim standing
  still is hit twice a cycle (once by the zero-frame ONA, once by ON), which
  the original's timings seem to do too but was not confirmed.
* Level tuning: each level record of a realm's data wad holds seventeen
  floats from +0x9C (`LevelTuningRecord`, unpacked as `tuning`): the player
  level it is meant for, experience and damage multipliers, the difficulty,
  then enemy, generator and trap scales, where zero means "the difficulty".
  `WorldData`'s `LevelTuning` keeps what is used so far: `damage` (scales
  every hurt over a point), `trapRate` (times run at 1 / (rate * gain)) and
  `trapDamage` (times gain). The gain is the game's difficulty
  (`game.difficulty` in the settings: easy 0.667, normal 1, hard 1.5, the
  original's). G1 is 0.75 and 0.5, so its spikes do 10.
* Harm and death (`PlayScene::hurt`, `blast`, `settleBlasts`): blasts hurt
  whoever is within their radius and strike the barrels in it (queued, so a
  barrel that blows up sets off its neighbours without recursion): a trapped
  chest 50, an exploding barrel 30, both over 12 units, a poison barrel a
  cloud of 6.5 units that does 10 every half second for four seconds, all
  times the trap damage scale. Cries follow the original's ids: fire `PAIN1`,
  spikes `DIE1`, gas `POISON`, blows `PAIN2` for every 30 taken, death
  `S_PLAYERDIES` and `DIE2`. Under a point of health the character plays
  `DEATH` and is then "in the tower" (`StatusBoxView::inTower`, the text
  `hud.inTower`): unseen, untouchable, out of the camera's view and not
  waited for by portals. `PlayScene::party()` hands the fallen on as they
  came into the level (`PartyMember::fallen`, the entry snapshot, keeping
  only the help they saw); they stand again when the party is next in the
  tower, and stay fallen through any other level. With everyone fallen and
  the last body gone, three seconds later the scene returns
  `PlayOutcome::Fallen` and `Gauntlet` takes the party to the tower. Nobody
  is hurt in the tower. Not yet: knockback, the low-health narrator lines,
  armour and shields reducing damage, gas spoiling food, blasts destroying
  pickups, breakable walls (item type 10, subtype 42).
* The turbo meter (`players/TurboMeter`, one per actor in `PlayScene`): what
  it holds climbs 2 a second to 100 while the character is free to act (not
  fallen, not in a turbo move), what is shown chases that a point a tick up
  and two down, and `look()` gives the original's picture of it: under 40%
  the front colour yellow (brightening through the zone) on black, then red
  on yellow, and at 99% red with `TURBO_GLOW_NEW` pulsing over 120 ticks; a
  change of zone plays `TRBO_GLEEM1..5` up and back, four ticks a frame.
  `StatusBoxPainter::drawTurbo` draws `TRBO_FULL_NEW` whole in the back
  colour and again squeezed about its middle in the front one, at y 304 over
  the box, with `TRBO_GLINT` on top and the gleam at (80, 310). The original
  has two buttons, and so do the settings: `turbo` (its TURBO/DEFEND: Left
  Shift, right bumper) held as the attack goes down is a turbo attack, the
  greater (`ATTPWRC`, costing 100) with a full meter, the lesser (`ATTPWRB`,
  40) with two fifths, an ordinary attack with less; `charge` (F, pad Y)
  going down is the charge (`SHOVE`, wanting 5, running the meter down 20 a
  second while it plays): the character rushes flat out at 1.5 times its
  pace the way the stick is pushed (past a quarter), else straight ahead,
  and what it runs into is struck for 3, once a charge
  (`PlayScene::chargeInput`, `ramBarrels`). Held by itself `turbo` is the
  guard: `DEFEND1` up, `DEFEND2` for as long as it is held, `DEFENDR` down;
  with too little in the meter for a turbo attack, turbo and attack together
  are still only the guard, as in the original. `PlayScene::guarded` is the
  original's rule as it shipped: a raised guard halves a hurt that comes
  from somewhere (a blast, gas) and takes all of one that comes from nowhere
  in particular (a trap underfoot, which then does not stun either); a
  charge halves either. A special pickup with flag 0x80000 fills the meter,
  `awardExperience` adds 0.025 of what is won (nothing awards any until
  there are enemies), reaching full posts help 110, dying empties it, and it
  starts every level empty (a scenario member may give a `turbo`).
* What a turbo attack does is the class's own data (`formats/PlayerDataWad`:
  the wad's SFXX and DAMG sections and the twelve move indices of its
  record; unpacked into `pdata/<CLASS>.json` as `moves`, `moveEffects`,
  `moveStrikes`; loaded into `ClassStats`; re-unpack with `--only PDATA`).
  A move names its first strike and strikes chain by `next`
  (`ClassStats::strikesOf`); the lesser attack runs `turboB`, the greater
  `turboC1` and `turboC2`. `PlayScene::beginMove` lines them up and
  `runMove` makes each as the move's sequence reaches its `startFrame`: its
  effects (trees of the costume colour's `PLAYERS/<CLS>/SFX<COL>` archive,
  chained by their own `next`, `NULLFX` showing nothing) start turned to the
  facing with their sounds, the meter pays what the move owes at the first
  strike that does harm (a move cut short before that is never paid for),
  and `world/MoveStrikes` carries the harm: a burst (type 4) reaches what
  is within its radius, and its arc when it has one, its delay after it
  starts; what flies (type 2) goes off along the facing at the mean of its
  speeds, harming what is within its hit radius as it passes, until its
  time is up or a wall ends it, its effect (`EffectTrees::startSet`, which
  can turn, carry and repeat an effect) going with it. A negative amount is
  that many times the character's own missile damage. Strikes reach only
  barrels so far. The warrior's: B a burst of 50 over 12 units half a second
  in, C a burst of 25 over 8 and at frame 9 a wave of 70, ten wide, at 30 a
  second for six seconds. What flies plays its effect's tree once and then
  its `loopEffect`'s tree repeats in its place (`EffectTrees::Setting::then`)
  until it ends. While a strike flagged 0x10, 0x20 or 0x2000 lasts (from its
  start frame to its end frame, or the move's end) the level goes dark by
  0.4, 0.6 or 0.8: `engine/world/AmbientDimmer` is the original's ambient
  special (asked for every tick, the light falls a quarter a 30 Hz frame,
  and once unasked what was wanted fades to three fifths a frame while the
  light climbs back a twentieth), and `LevelWorld::setAmbientOffset` applies
  it to everything lit: the level's geometry, whose light is baked into its
  vertices, through `WorldScene::setDarken` and `DrawState::darken` (a
  multiply in the fragment shader, additive parts left alone), everything
  lit as it is drawn through `lighting()`; effects are drawn by
  `fullLighting()` so that they stand out. (Taking it off the ambient term
  alone shows nothing: a level's light saturates whatever faces it. The
  original also feeds the value to its screen windows, which is what makes
  the whole picture darken there.) The effect trees' main parts are
  flip-books of meshes (type 2 "OANIM" nodes with `objectFrames`); the
  player archives must be unpacked with a gdlunpack that writes them, or
  the projectile and half of each burst are simply not there.
  Every class's moves run from its own rows, the unlockable ones' too: they
  have no `ANIM` or `SFX<COL>` folders of their own, and
  `PlayScene::classFolder` gives them those of the class they shadow
  (`character % kStartingClassCount`), which hold their sequences, their
  thrown weapons (`MIN_THROW1` is in `WAR/SFX<COL>`) and the trees their
  rows name. Besides bursts and what flies, a row may be a span that only
  lasts (type 0: with flag 0x400 the hand is empty while it does,
  `MoveProgress::weaponHidden`) or a volley (type 10): while it lasts the
  character's own missile is let fly every `delay` frames, off the facing by
  the row's `angle`, which closes from full to none across the span with
  flag 0x200 (the archer's Double Bow is two such, from a quarter turn to
  either side) or opens with 0x100 (`runVolley`, `launchWeapon`). A strike
  that harms something shows its `hitEffect` there. The strong attack
  (`strongAttack`: R, left bumper; the original's SLOW ATTACK) is, with
  nothing in reach, the strong throw: `ATTPWRATHROW` at a quarter pace, the
  weapon leaving as it ends at twice the size and the harm
  (`MissileLaunch::scale`), then `ATTPWRATHROWR` with the hand empty; its
  rows (`turboAThrow`) give its sound, and it costs the meter nothing. Its
  melee variants (close, low, step, 360: `turboAClose` and the rest, at
  half to one and a half times the character's own harm) need something in
  reach, so they wait for enemies. `PlayScene::awardExperience` is the
  original's award: scaled by the level (`LevelTuning::experienceScale`:
  the place's own scale, G1's 2.85, less the further the character is past
  the level it is meant for), a kill's feeding the meter 0.025 of what was
  won unless a turbo move is under way; only enemies, critters and
  generators ever award any, so nothing calls it yet. A guard that takes
  more than 2 off a hurt shows `BLOCKFX` for 0.01 s a point that got
  through (a third of a second to one), and not again until it is over
  (untinted: the original colours it by class at a quarter alpha).
  Not yet: the strong attack's melee variants, damage types (the element,
  knock-over) doing anything, the directional guards (what selects them was
  not found; they look like answers to where a blow comes from), and the
  two player combo (a grab, carry and throw system of its own, help 111
  with it).
* More of what a player can do. Strafing (`strafe`: Left Control, left thumb;
  the original's STRAFE): while it is held and the stick pushed, the
  character steps that way with its facing held (`PlayerActor::update`'s
  `keepFacing`), in the `STRAFE_WLK<F|B|L|R><1|2>` half cycles picked by the
  step's heading against the facing (within an eighth of a turn of ahead or
  behind, else the side, a positive turn being the original's right:
  `PlayScene::strafeWayOf`, `PlayerAnimator::setStrafe`); with the attack
  held too it goes on stepping in `STRAFE_ATK..` and lets its weapon fly as
  each half begins, its feet never planted. The shield potion
  (`shieldPotion`: C, right thumb) is raised with the gesture of a potion
  used (`PlayerDeed::ShieldPotion`, `potionShielded()`): the potion's
  `MS_FIRE|ELEC|LIGHT|ACID` tree rings the character for three seconds,
  going about with it (`EffectTrees::moveTo`), sized like a burst, to
  `S_SHIELD1..4`, and harms what it touches for a quarter of the magic power
  every half second (ours: the original's is an effect with a damage radius;
  it takes nothing off what the character is dealt). A blast that gets more
  than a point through and finds no guard floors its victim: onto its face
  (`FALLFRNT`, then `GETUP2`) when it came from behind the way it faces,
  onto its back (`FALLDOWN`, `GETUP`) otherwise, heeding nothing until it is
  up (`PlayerAnimator::floored`, part of `reacting`). Not yet, because they
  need something to be aimed at or to come from: melee (the quick and slow
  attacks against what is in reach, their combos and directions, which the
  original resolves through its enemy targeting), the knock-back slide,
  falling from ledges (`FALLING`, `LAND`: the actor still refuses a step
  with nothing under it), pushing, webs, grabs and Death's, the victory
  pose, the super shot and the familiars' attacks.
* The swarm (`game/enemies`). `EnemyKinds` is the original's per-kind table
  (thirty-four rows: size, pace, damage, health, armour, the experience a hit
  and a kill are worth, the way each goes about, all of it its own, none of
  it player-facing); a kind is looked up by the name a level's generator
  gives it ("GRU", "RAT"), but that name is a stand-in for a class, as the
  original's `GetEnemyType` has it: a rat for the small, a grunt or a knight
  for the medium (the medium's second row at strength four and over), else
  the large, and the level's roster (`LevelInfo::enemies`, the realm's
  `ENMY` table of WDATA, kind and class, indexed by the level record's six
  rows at +0x4C) says which kind fills each (`levelKindOf`). So the fields'
  "GRU" generators breed zombies out of graves (`GEN_ZOM3`), its "RAT" ones
  maggots, and a name of no class (the great ones) stands for itself; the
  grunts are the castle's. `EnemyAnimator` is the original's action
  dispatcher: thirty-three actions asked for through a tick by priority
  (Ready 100, the walks 200, attacks 300, hits 400-460, dying 999), the
  loudest winning and the tick's request forgotten after; a swing chains into
  its recovery and its blow lands at that moment (`struck()`,
  `powerStruck()`), a hit cuts into anything, a death the tree lacks plays as
  the knock-down (the grunts have no DEATH). `Enemies` is the pool of
  twenty-five (a level's `maxEnemies` of them: G1 thirteen), the body: it
  chooses a target every eighth frame from the players within sight (thirty
  at a sight scale of one, each mind choosing adding two to that player's
  distance for the next, so a party is shared out), gathers a `MindSense`
  (where it is, what it sees, what it bumped, two probes: `clear`, whether a
  step crosses anything, and `open`, whether a step keeps the body off the
  walls) and carries out the `MindIntent` its mind returns (a heading, a
  pace, whether to turn, the action to ask the animator for, perhaps a change
  of mind or that it is done with). The minds are `EnemyMind` strategies in
  `EnemyMind.cpp`, one class per way of the original's `do_ai` switch,
  looked up by way number through `enemyMindOf` (strangers wander): seek
  (0) straight at the player or the nearest clear sixteenth either side;
  prowl (2, the rats' and the other small kinds', whatever a generator asks)
  wandering until a player is within eight, then seeking for good; wander
  (5, 6) straight on, an eighth of a turn round at a bump, held thirty
  ticks; chase (7) straight while the way is open, else skirting on the side
  the nearer probe gives (a sixth of a turn either side of the facing), a
  sixteenth of a turn further from straight until a step is open, until the
  straight way is open again (the original tries the same offsets but only
  after each dead stop and slides back under the player between them: it
  jitters behind a wall, which we do not copy); a dead stop holds the
  heading ten ticks (fifteen for another enemy) and counts a bump, seven
  bumps and the route doubles back, ten refused headings and it goes
  straight anyway; loiter (11) turning on the spot, done with when its
  generator is gone; flee (24) away at a run; lurk (27) still until a player
  is within sight, then seeking for good; stand (31) facing whoever comes
  against it; throw (17, 23) standing, facing its player and throwing
  whenever they are within sight and ten above or below and its wait since
  the last throw (the placement's fourth param in ticks, `idleTicks`) is
  over; skirmish (16, the archer's) the same, but backing off, weapon up
  (`RUNATTACK`, which runs on), when the player is within six tenths of its
  sight until beyond eight tenths; suicide (18) still until someone is in
  sight, sixty ticks of fuse, `READYTOWALK` to light it, then a run at them
  half as fast again, blowing up against them or after two hundred and
  forty ticks: a blast of fifty at the level's enemy damage over the chest
  radius, and dead of it. A mind keeps what it needs between ticks in the
  enemy's `MindMemory`; a slide along a wall that still gets somewhere is
  no bump, only a dead stop is, and the touch is remembered while the hold
  runs. A placement of strength four, five or six is the archer, bomber or
  suicide variant (`EnemySpawn::tier` past the tiers): the kind's `<PREFIX>A`,
  `B` or `S` tree, the archer's and bomber's health the second tier's, the
  suicide's the first's, each with its own way unless the placement gives
  one. What they throw is `EnemyMissiles`: the original's table (0x80119128,
  0x30 a row, three slots a kind) gives every medium kind the same shot
  (ten damage, twenty-five a second, half a unit wide) and lob (ten, twenty,
  knock-back, a burst of three, spinning about y); a shot flies straight at
  the player's middle from the kind's attention height at the level's
  `enemyMissileSpeed`, a lob leaves so as to fall there under a gravity of
  forty (ours: the original leads and weights it); either strikes the first
  player its body meets, or the world, and a lob bursts either way. The
  models are the kind's `<PREFIX>_ARROW` and `_BOMB` trees (the zombies'
  arrow is a pitchfork). `enemyMissileOf(kind, slot)` is the whole table
  (the demons', ghosts', plague's, sorcerers', warlocks' and garm's bolts of
  their own in the third slot, the worm's three) and `missileSlotOfWay` the
  original's slot by way (16/23 the first, 17/26 the second, the rest the
  third); a kind with nothing in its slot throws the arrow. Against a player the body stops and asks for the attack
  (every eighth the power one), the blow landing as the swing ends whether or
  not the player is still there: the kind's damage, tiered down to two thirds
  and a third as its health falls under those shares of its kind's full
  (so a second-tier grunt, its health a third short of full, always hits for
  ten), the power blow half as much again and, from a body reaching over two,
  a knock that makes the player flinch. A hit on it (`EnemyHit`: the
  original's damage-type bits, 0x10 knock-back, 0x10160 or a magic hit over
  ten throwing it down) takes armour off, a character always getting a point
  in, scaled a hundredth softer a level under the place's `playerLevel` and a
  tenth harder a level over; it flinches or is thrown back (forty a small
  body, twenty a tall one, capped at forty, decaying by 0.8 a tick) and gets
  up, and dead plays out its fall and is gone. Experience is the kind's hit
  or kill share through `awardExperience`. A slot is found first empty, else
  the least worth keeping (the furthest from its player, a dying or sleeping
  one a hundredth of that, an unseen one ten thousand dearer), never a
  stronger one for a weaker. `Generators` are the level's type-3 items:
  params little-endian s16s strength (the tier bred and how many records of
  health), way, count and interval (defaults 10/5/2 and 5/10/15 by tier), the
  count and interval scaled by the level's `generatorMost` and
  `generatorRate` (with the difficulty gain) and truncated whole, the health
  by `generatorHealth`; the countdown is six ticks a unit of interval,
  stretched by a share that grows 1/(2 x count) a birth and wraps at one; one
  breeds only with a player within forty-eight (ours, for the original's
  on-screen test); a birth goes in one of the eight octants about it, the
  humanoids only ahead, at its height plus the body's radius out, where the
  floor is within six, no wall, player, enemy or box is in the way; a state
  crumbles at each record of health (three whole, `GEN_<PREFIX><state>L1`
  objects of the kind's archive, the kind's GENHIT/GENDIE trees over it) and
  gone it frees its brood. Level placements (type 4) of ordinary strength
  stand where put, asleep at nought. Level tuning's enemy and generator
  columns are in `LevelTuning` (`enemyHealth`, `enemySpeedScale(gain)` and so
  on: what they take and deal is the level's own, speed, sight, rate and
  count grow with the gain). Missile targets are the barrels by their ids,
  enemies from 1000 and generators from 2000; strikes and blasts reach them
  too, with the strike row's damage type. gdlunpack's `--only MONSTERS`
  unpacks the seventy-two monster archives (`GENERAL` one folder a realm).
  Not yet: the other minds (guards 8, milestone routes 10, the ghosts' 19,
  the kiting of 26/28/29 and the rest), the original's missile lead and
  weight, the arrow's and bomb's hit effects and sounds, Death, IT, gibs
  and the enemies' sounds (a generator struck or destroyed sounds the
  realm's `S_GENDAM<letter>` / `S_GENKILL<letter>`), melee for the player
  (now that there is something to hit), and the original's on-screen gate
  on breeding.
* The great ones (`game/enemies/Critters`, `CritterData`,
  `formats/CritterWad`): golems, generals and gargoyles, the original's
  critters, whose minds are data. `gdlunpack --only CRITTER` writes
  `critter/<NAME>.json` from `CRITTER/<NAME>.WAD` (a data wad like PDATA:
  TYPE 0x140, MOVE 0x90, DAMG/NODE/SFXX 0x50, DESC 0x30; the fixed text
  fields keep the packing tool's leftovers past their first nought, so a
  colnode of a tab names no node). `CritterData` is the first type's table:
  size, wall radius, armour, health, its value in experience, its sight, its
  moves, damages and parts. A `Critters` pool of sixteen stands one at a
  placement (kinds 29/33/32 of the level's type-4 items) with the realm's
  costume (`MONSTERS/GOLEM/LEVELG`, `MONSTERS/GENERAL/LEVELG`; a gargoyle
  its form's, `MONSTERS/GAR_EAGL`) and the tree of prefix plus suffix
  (`GOLEM1`), and health `maxHealth` at the level's enemy health. Each tick
  the move it is doing plays; done (or something louder come), what it links
  to plays, else the loudest move whose target rule (`CritterTarget`:
  distance window, a cone `minDot` wide pointing `yaw` from ahead, so TURN's
  points behind and the gargoyle's LEFT/RIGHT to the sides) and cooldown
  allow it: attacks (types from 128) over walks (52) over the stance (32);
  a move carries the body at its own `speed` a second (the type's speed is
  a cap, not a pace) and turns it at its `turnRate`, never onto a player or
  another; over its harmful frames (`frameStart..frameEnd`, a second window
  too) its damage record strikes: a blow (0) whoever is within the part's
  radius plus its reach of the named node's posed position, a ring (3, the
  stomp) or a breath (4, in its cone) whoever is within its reach of the
  feet, each player once a move, for `damage` at the level's enemy damage.
  A hit takes the armour off (a point always through for a character), a
  block lets a quarter through and shrugs off the throw, and is worth
  amount / (1 + health) of the value to the hitter (a fiftieth less a level
  under the place's `playerLevel`, never under a tenth), paid in whole
  points as they add up; fifty taken and it roars; a floored hit plays KD
  (KB otherwise) and throws it (twenty, a golem five less); dead it plays
  DEATH, fades a second and is gone, a fifth of its value going to everyone.
  A gargoyle slain leaves the key its form is named by (`GARGEAGL`) where
  it fell. The bosses are critters of type 4 (`kBossCritter`, by name:
  `bossNameOf(bossType)`, the dragon 34 to the garm 44; archive
  `MONSTERS/<NAME>`, tree prefix plus suffix, `LICH`): the level's
  `bossType` stands one at its `boss` mark, and it fights by the same
  table: the step family (types 48 to 63: turns, walks, back-steps) is
  chosen like the walk, and a named attack of speed (the lich's `CHARGE`)
  carries it. Not yet: the bosses' patterns (PTRN), phases, wake threshold,
  health meter and cameras, their children (the chimera's heads) and
  projectile moves, the general's waypoint patrol, the gargoyle's fireball,
  per-part damage and breaking, the critters' sounds, and the statue's
  waking.
  Scenarios: `level-g1-general.json`.
* Levels gained (`players/LevelWatch`, `PlayScene::updateLevels`). The watch
  is told each player's level every tick and reports the changes since it
  last looked (`LevelChange`: from, to, `milestone()` when a tenth is
  crossed), so what grants experience knows nothing of levels and what
  answers one knows nothing of experience. A level gained does what the
  original's `AddExp` does: help 34, "LEVEL %d" (the number filled in by
  `HelpMessages::post`'s `number`; `HelpRepeat::Always`, news not a lesson),
  to `S_GAINEDLEVEL`; the `LEVELUP_<COL>` tree of the weapons archive about
  the character for three seconds; a hundred health. A tenth level besides
  says the class's piece (`S_EXP10WAR` ... `S_EXP90WAR`, `S_EXP99ALL`
  failing one) and reloads the figure in the tier's costume (`BLU10`) where
  it stands. The watch is primed at open with the party's levels.
* A bitmap an archive flags 0x100 has no picture of its own (an animated
  texture's slot, such as the magic users' `<COL>_HANDGLOW`, filled in the
  original from frames kept elsewhere): `TextureSet` draws it clear
  (`TextureSetEntry::noPicture`) instead of failing, which used to keep
  every wizard, archer and the like from being built at all. A file that is
  simply missing is still an error.
* Barrels (`world/Breakables`): item type 10 subtypes 43 plain, 44
  exploding, 45 poison, and the containers of subtype 43 that hold an item.
  Hit points and armour come from the record (5 and 1): a blow takes its
  power less the armour, never under one. `PlayerMissiles::update` takes
  `MissileTarget`s and reports the one an impact stopped against, with the
  missile's damage (5 to 20 by the thrower's stat). Broken, a barrel plays
  ACTIVE, stops blocking, and leaves its staves (DONE) or, having blown up
  or gassed, nothing. Sounds are the realm bank's `S_BARREL_WOOD`/`_EXPLO`/
  `_GAS` plus the realm's letter, `S_WEAPONHITWOOD` for a blow it survives.
* Help messages (`screens/HelpMessages`): the original's table of message
  ids, each a message of the game's strings (`text/english.json`, whose
  lines are the box's lines) and a narrator line from the `VOICE1` bank,
  drawn in the strings' own small capitals on the scroll sheet at half
  alpha, centred 62 pixels over the character's head, for a second a line
  plus half a second, one at a time, with a growing pause after each (0,
  120, 240, 420, 600 ticks). A message goes up until every character in play
  has seen it (`CharacterSave::helpSeen`, saved); `HEALTHFULL` is per player.
  Wired so far: door and chest wanting a key, keys full, no potion, health
  full, traps, barrels that hold things, chests that explode, the meter
  coming full. The shown flags have two halves in the original, "ever" (the
  save's `helpSeen`) and "since this character was loaded" (cleared on
  loading; here `PartyMember::helpHeard`, carried from level to level and
  never saved), and `HelpRepeat` says which a message goes by. The names of
  the classes' turbo attacks (ids 57 to 79, three to a class: the strong
  attack has none, then the lesser and the greater, e.g. the warrior's FIRE
  ARC and PLASMA TRAIL) come once a session, a frame into the move (the
  move's strike row names the id), as one line of the class's
  `<CLS>_TURBO` text with the announcer's line from the class's own bank,
  at a priority (60, 70) that takes the place of a lesson (50) already up.
* Back from a realm the party stands at the tower's start marker among that
  realm's portals: `LevelWorld::towerMarkerOf` is the original's realm to
  marker table (town 7 -> 1, mountain 2 -> 2), not the realm id itself. There
  it materialises as anywhere (the spawn effect, the START sequence, the
  level's title) with the follow camera already on it: the start camera's
  hold and ride in belong only to a party standing at the level's own
  entrance (`arrivalPoint(...) == startPoint(0)`), so neither a party out of
  a level nor one that fell is shown Sumner's hall first.
  Scenarios: `level-g1-chest.json`, `level-g1-gate.json`,
  `level-g1-trap.json`, `level-g1-nokey.json`, `level-g1-barrel.json`,
  `level-g1-death.json`, `level-g1-turbo.json`; in the tower
  `tower-turbo-archer.json`, `tower-turbo-wizard.json`, `tower-strafe.json` and
  `level-g1-generators.json`. A scenario's `position` is not checked against
  walls: pick open ground from the level's collision.
* Texture wrapping is per axis (`TextureDesc::wrap` across, `wrapV` down,
  `TextureSetEntry::clampU`/`clampV`, eight Vulkan samplers): levels clamp
  ground and wall textures one way only, which the tower never does.
* Inventories and items: `players/Inventory` (in each `ClassProgress`, saved
  under `inventory`) holds keys (9 at most), potions by kind (9; the last
  taken shows and is thrown next) and eleven `PowerupSlot`s filled the way the
  original's `PlayerAddPowerup` does (the same kind and flags renews: all its
  charge, half its strength; else a free slot, else the weakest; strength
  under none is for good). `players/ItemPickup::takeItem` ports the pickup
  rules by item subtype (1 gold to 99999, 2 keys, a ring leaving what does
  not fit, 3 food up to `mostHealth(level)` = 500 + 100 a level, refused at
  full health, bad food always and never past the last point, 4 potions, kind
  from the record's properties, 5-9 powerups at the class's `powerupTime`)
  and names the card (`KEY`/`KEY_RING`, `MEAT`/`FRUIT`/`BADMEAT`/`BADFRUIT`,
  `MAGIC`, `GOLD`/`JUNK`, `SPECIALS`) and the sound (`S_PICKUPKEY`,
  `S_PICKUPMAGIC`, `S_PICKUPSPECIAL`/`S_PICKUPSHIELD`, or the class's own
  `S_<FAM>EATSFX`/`PAIN1`). `PlacedItems::collect` takes a `PickupJudge`
  (nothing = leave it lying, else what is left of its amount), and
  `PlacedItems::place`/`LevelWorld::placeItem` drop an item by the name of
  one of the level's item records (the tower's records cover keys, food,
  potions, treasure and coins though it places only crystals), which is how
  chests and fallen enemies will leave theirs. The status box shows
  `KEY_ICON` and the next potion's `POTION_ICON_<COL>` with their counts over
  the gold and health. Scenarios take `gold`, `health`, `keys`, `potions` per
  member and `items` (`name`, `position`); `tests/scenarios/tower-items.json`
  lays a spread out. Powerup timers do not run in the tower (nor did the
  original's).
* Using what is carried: `PlayBindings` adds `usePotion` (E, pad B),
  `throwPotion` (Q, pad X) and the selector's four presses (I/K/J/L, the
  pad's directional buttons, which therefore no longer walk by default: the
  stick does, as in the original). `PlayerDeed` tells `PlayerAnimator` what
  the buttons ask; a potion plays `MAGICS` then `MAGICR` (used) or
  `THROWPOTIONS` then `THROWPOTIONR` (thrown), the release's start being
  `potionUsed()`/`potionThrown()`, one potion a press. `PlayScene` takes the
  next potion (`Inventory::takePotion`) and bursts it through
  `world/EffectTrees` (an archive tree played once at a place and size; the
  WEAPONS trees `MP_FIRE`/`MP_ELEC`/`MP_LIGHT`/`MP_ACID` for red, blue,
  yellow, green, sounds `S_POTION2/1/3/4`) sized 0.03125 x magic power
  (`PowerupEffects::magicPower`, 8 to 32 by the magic stat, at most 1), or
  throws the bottle (`POT_<COL>_TW`) as a `PlayerMissiles` missile from 4 up
  and 2 ahead at 5 x 0.707 up and forwards, bursting at 0.75 of that power
  where it lands (`MissileImpact::potion`). `players/PowerupEffects` gathers
  the worn slots (`PowerupSlot::on`, toggled by `screens/PowerupSelector`:
  up opens after a 128-unit slide at 4 a tick, left and right go round, up
  switches, down closes; its label is `powerupTextId`'s text, glowing while
  worn) into weapon, armour and special flags plus pace and magic adds; so
  far the scene applies speed (`PlayerActor::setPaceBonus`), three and five
  way shots (`PlayerMissiles::spread`, 15 degrees apart), invisibility (body
  alpha 95/255 wavering) and growth (1.3; an ogre is 1.6, level 99 1.2).
  `Inventory::spendKey` is the rule for locks; chests and doors come with
  the realm levels (the tower's archives hold no chest or door). Scenarios
  take `powerups`; `tests/scenarios/tower-powerups.json` carries a set. The
  shield potion, the other powerups' effects and the full-screen inventory
  are still to come.
* Attacks (first slice, the throw): `PlayBindings::attack`/`padAttack`
  (Space, A) held is `PlayInput::attack`. `PlayerAnimator` ports the
  original's throw actions: the wind-up (`THROW1S`, or `THROW2S` cut in from a
  first half of walking or running) gives way to the release (`THROW1`/
  `THROW2`) at its end or at once from frame 2, the release's end is
  `released()` (the weapon flies), and the recovery (`THROW1R`/`THROW2R`)
  leads to the next throw or back to the stance; a throwing body's
  `moveScale()` is 0, so `PlayerActor::update` turns it to the stick without
  moving it, and the hand is drawn empty while `recovering()` unless the
  class's `MissileSpec::staysInHand` (staffs, bows). `world/PlayerMissiles`
  flies them from the original's tables (`MissileSpec`: tree
  `<AXE|SWD|STF|BOW|HAM|MAC|WND|BOM|MIN|FAL|OGR|UNI>_THROW<tier>`, tier '0' in
  the costume archive, else by level in `PLAYERS/<CLS>/SFX<COL>`; radius,
  tumble 18.85 rad/s, weight as gravity): pace 20..60 by strength (magic for
  the wizard and sorceress families), launched from the body's centre plus the
  class record's `weaponOffset` (now unpacked into `pdata/<CLS>.json`; re-run
  `gdlunpack --only PDATA`) and 2 ahead, lobbed to land 0.5 under its start at
  a reach of 15 (+200 per second held past 0.27 s, at most 0.1 s), stopped by
  walls and floors (`takeImpacts`, nothing drawn for them yet) or after 3 s.
  The throw sound is `S_<FAMILY>THROW` from the class family's bank. Melee,
  damage, targets, aim assist, streaks, spread shots and impact effects are
  still to come.
* Sumner's hints: a player inside the trigger before him (id 240,
  `PlayScene::kSumnerSpot`) is greeted at once (`SumnerFigure::play`, the
  original's sequence indices: 3 WELCOME, 4 GOAWAY, 6 GESTRIGHT) and handed
  `menu/HintMenu` two seconds on, once a visit (leaving the spot starts a new
  one). The scroll holds play like a message scroll; its owner's menu input
  drives it. `world/SumnerHints` ports the original's four hint pickers over
  `text/hints_e.json` (`MessageTable` lists: `GENERAL_HINTS`, `BOSS_HINTS`,
  `LEGEND_HINTS`, `RUNE_HINTS`; titles from `BOSSHINTDESC`,
  `LEGENDHINTDESCS`, `RUNEHINTDESCS`): general hints carry on across visits,
  the other three start over each visit. `HintKnowledge::ofParty` knows only
  which worlds the party's crystals open; runestones, legend items, beaten
  guardians, gargoyle wings and the tries that earn extra passages are fields
  waiting for saves to track them. A hint page is an `OptionMenu` without
  items whose `MenuDefinition::body` passages are written in ink; a lone
  prompt sits mid-row. Backing out of the topics burns the scroll
  (`menu/BurnDialogueScroll`, the burn every scroll and the options menu
  share) and Sumner waves the player off. The labels are `hints.*` in
  `data/text/en.json`; `tests/scenarios/tower-sumner.json` starts before him.
* Sumner's beam (`L1XPLIGHTRAY01`) starts unseen and comes up over 180 ticks
  while a player is within `kBeamRadius` of him, going again once they
  leave (`PlayScene::updateBeam`). The stained-glass light through the
  window over the door (`kTempleLights`) starts dark: it is the Desecrated
  Temple's, lit once its shards are all found, and the save keeps no shards
  yet.
* The welcome's cut to the crystals is letterboxed as the original's
  trigger cameras are (`kCutBarTop`/`kCutBarBottom`: 48 and 80 of the 384
  canvas rows) with the status boxes hidden.
* Item reach: a character takes an item within `PlayerActor::reach()` (the
  class's whole width, twice the footprint walls stop) plus the item's own
  radius sideways, and within the item's height plus half the character's
  height up or down (`Collector::height`).
* Entering the tower: the party materialises in the `STARTFX` tree of the
  `WEAPONS` archive (its `CHARWARP` texture animation playing, for
  `kSpawnTicks`) held still under the level's title while the
  `StartCamera` holds at the `cameraStart` marker (91 ticks, a button
  cutting it short once under 45 remain) and then rides to the follow
  camera at `StartCamera::kUnitsPerTick` (a quarter unit a tick; the
  original's unit a tick snaps over the tower's short ride), its look-at
  point sliding along; ticks are real time, so the ride is the same at any
  frame rate. The party plays its `START` entrance through the hold. The
  title sits centred near the top, sliding up over the hold. Then the
  welcome scroll, when one is due. A party placed by
  `PlayOptions::position` skips the ride. The realm's entering sound (`WorldData::soundName` of
  `audio->enterSound`) belongs to the loading screen and is not played here.
* Gate messages: `LevelTriggers::takeRefusals` reports a player stood in a
  requirement trigger without what it wants (once per 2.5625 s per trigger)
  and `takeOpenings` the targets that opened; `PlayScene::handleTriggerEvents`
  opens the `NEEDCRYSTALS` page for the realm or the `NEEDGARGITEMS` page for
  the gargoyle tier (the trigger id less `kIconTierBase`, 101) from
  `text/scroll_e.json`. A target opening before the party sounds by the
  slot its trigger names (`LevelTrigger::sound`, the instance's sixth
  parameter; 255 is none): 0 the force fields and magic crossings, 1 the
  lifts, 2 and 3 the east and west gates, each a `kOpeningSounds` pair
  played while it opens and once `takeSettled` reports it done (faded to
  nothing or its animation at its end). The tower's ambience bank keeps
  those samples (`ffield`, `lwrtwr`, `eastgat`, `westgat`) under the audio
  directory's elevator slot names; a Dolphin recording matched the field's.
  `collectItems` announces a realm's gate opening once (the
  `UNLOCKLEVEL` page and the `S_CRYS4*` voice from the level bank, kept in
  `m_voice` and cut off when the scroll is left), and
  `ClassProgress::unlocked` (a bit per realm, in the save) keeps it from
  repeating. Any open scroll pauses play, and leaving one burns it to
  `S_OPTMENUSCROLL`, the options menu's note.
* Object animations: a tree's object node (`TreeNodeInfo::kObjectType`, the
  original's `XCOANIM`/`OANIM` nodes) carries `objectFrames`, one run per
  sequence (`objectFrames` in `animations.json`, from the tree's third header
  field): the run's first object is shown at `start` and each frame after it
  the next object of the archive, for `frames` frames, then nothing (a
  one-frame run stays). `TreeModel::setFrame(sequence, frame)` picks the mesh;
  before it an object node draws nothing. The spawn effect's flame column
  (`STARTFX0F01..0F25`) is one.
* Items play their archive's texture animations (`PlacedItems::ArchiveMotion`
  with a `TextureAnimator` per archive, applied through
  `TreeModel::setTextureFrame/setTextureOffset`): the sheen scrolling over
  the crystals.
* Frame rate: `Application::setMaxFrameRate`; the menus run at
  `display.maxFrameRate` (60) and play at `timing.gameplayFrameRate` (30),
  as the original did. Play at speed needs the release build
  (`build.py --run` uses it); the Debug build runs the tower at about a
  third of the rate.
* Back (Backspace, B) does nothing in play; the tower is never left by it.
* Input: `Input::latchKey` (fed by the GLFW key callback) keeps a press that
  came and went between two polls down for the next poll, so a tap shorter
  than a frame still moves the character.
* Text: `TextPainter` samples each glyph cell half a texel inside its borders
  (`kCellInset`), so filtering never pulls in the sheet's grid lines.
* Sound banks: `gdlunpack` unpacks every `.vbk` in the audio folder; a bank
  the audio directory (`AUDATPS2.ROM`) does not name (`LEGACY_BARRIER`,
  `LEGACY_GEN`, `POJO`, `DIAGTUNE`, `SHOP_DON`) gets sounds numbered
  `<BANK>_<nn>`, each as long as its clips, with ids of -1.
* Textures: `gdlunpack` bleeds each opaque colour into the transparent texels
  beside it (`Image::bleedIntoTransparent`) so cut-out edges filter into the
  texture's own colour rather than the black the console files hide behind
  alpha; re-run the unpack after changing the decoder.
* Triggers: `game/world/LevelTriggers` reads the layout's trigger items (type
  5): the target object from the instance's first parameter word, the trigger
  flags from its second (0x40 = wants the realm the id names, the kind's
  low byte drives the target: 0x10 fades), the radius from byte 4 (half units,
  0xFF a hair, 0 the item kind's own radius), the id and next id from bytes 6
  and 7. Targets that the layout animates are held at their first frame by
  `WorldAnimator::hold` until `fire` plays them once; chains follow next ids
  among triggers wanting no crystals, and a trigger chained after another
  (`LevelTrigger::chained`) is never set off by a player standing on it, only
  through its chain (the lion statue's chain starts on a spot at its feet); a
  crystal gate's spot reaches twice its radius (`kMetReach`) for a party that
  qualifies; `openMet` opens at level start whatever the party qualifies for;
  fading targets lose their collision (`WorldCollision::setSolid`) and thin
  out through `WorldScene::setObjectAlpha`. `PlayScene` passes the party as
  `TriggerVisitor`s each frame.
* Collision follows animation: level files keep the triangles of any object
  flagged to move (`WorldObject::kAnimated`, set on the animated and on the
  force fields that only fade) in the object's own space, the rest in world
  space. `WorldCollision::setMovingObjects` takes those aside and
  `setObjectTransform` places them from the scene's `worldTransform` after
  every animator step (`LevelWorld::syncCollision`).
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
  `PartyMember`s and `PlayOptions`, and `--scenario <file>` opens the tower
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
  `game/world/LevelWorld` takes its light, camera range and sound names from
  it instead of constants.
* Sumner's welcome: `assets/MessageTable` reads an unpacked text rom
  (`text/scroll_e.json`: fonts, messages with pages). `game/menu/ScrollBox`
  is the original's controller message box: the scroll art sized to the page
  (text plus 96, no narrower than the prompt plus 32, at most 512, centred on
  (256, 160)), the text centred 32 below its top with 4 between lines, the
  glowing prompt and button icon 8 below the text, a 15-tick hold before a
  page takes any joined player's button, and the burning scroll after the last
  page; the prompt is a string-table entry. `game/world/SumnerFigure` is the
  GWIZ tree of `ITEMS/LEVELL` at the event marker whose parameter is 0,
  cycling READY, READING and THINKING as the original's index does (advance
  whenever a sequence ends or changes, wrap past 2) and cutting to GESTRIGHT
  (index 6) on request. `PlayScene` runs the welcome for a party with no
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
* The original's longer texts are in the string table too, so that they can
  be translated: every message of the scroll texts (`scroll.<name>.<page>`:
  Sumner's speeches, the unlock and rune scrolls, the level scrolls), of his
  hints (`hint.<name>.<page>`) and the help messages in use
  (`help.<name>.<line>`), names in lower case and pages from 1.
  `MessageTable::translate(strings, prefix)` swaps a message's pages for the
  table's after loading (as many as are numbered without a gap, so a
  language may use more or fewer); fonts, scales and lists stay the rom's,
  and a message the table lacks keeps the rom's words. A test holds
  `en.json` word for word to the unpacked roms; a help message added to
  `HelpMessages` needs its lines added to `en.json` as well.
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
  streams; the game calls `SoundPlayer::update()` once per frame. A stream
  resamples along a Catmull-Rom curve through four frames and slides every
  gain change (volume, pan) over `AudioStream::kGainRamp` (5 ms); movie audio
  and sounds stop through `AudioStream::stop()`, which fades over the same
  ramp and drops what is queued past it (`SoundPlayer` reports a stopped
  voice silent at once). `AudioMixer::mix` holds the sum under full scale:
  a peak turns the mix down at once and it comes back over `kRelease`
  (50 ms), so many sounds at once no longer clip and crackle.
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
  headless machines. Tests must pass on Windows and Linux. Three tiers: the
  plain unit tests (what CI runs), the `[assets]`/`[unpacked]` tests (need
  the game data; they skip without it, so they only prove anything on a
  machine that has it), and the `gpu` label. Behaviour in the game is
  verified against the unpacked data in a test, or by launching a scenario
  from `tests/scenarios/` (`gauntlet --scenario <file>`) and looking; keep
  both where the data exists, CI cannot see it.

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
  add shell or PowerShell scripts. `scripts/setup.py` gets a fresh machine
  ready (finds or installs the compiler, CMake, Ninja, vcpkg and the Linux
  packages, then builds; `--check` only reports); when a new tool or package
  becomes a requirement, teach it to `setup.py` and the README's table.
  `scripts/devenv.py` resolves the
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
* CI (`.github/workflows/ci.yml`) runs `setup.py` then builds and runs the
  unit tests on Windows and Linux, lints on Linux, and tries the `gpu` tier
  on a software Vulkan device under Xvfb (best effort). The game-data tier
  runs only through the workflow's `game_data` switch on a self-hosted runner
  labelled `game-data` that has the disc extracted and unpacked
  (`GDL_ASSET_DIR` and `GDL_UNPACKED_DIR` in its environment). Never put the
  game data anywhere CI could see it.
* vcpkg pins its baseline in `vcpkg.json`. `VCPKG_ROOT` must be a git
  checkout at or after that commit, bootstrapped so the tool matches its
  scripts; the copy bundled with Visual Studio is too old and the dev-shell
  script steers around it. Bump the baseline rather than patching ports; if a
  port must be patched, add `vcpkg-overlays/` plus `vcpkg-configuration.json`
  and say why in the overlay's README.

## Git

* Do not commit or push unless asked. Never commit anything under `assets/`.
