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
* `screens/PlayerRuntime` owns each participant's actor, optional
  figure, entry save/slot, life state, turbo move, reactions and cooldowns.
  Keep per-player state in that record, not in parallel index-aligned vectors.
  Party indices, player input ids and save slots are distinct identifiers.
  Camera subjects are frame-local snapshots taken after player movement and
  before fixture collision; changing that phase changes camera behaviour.
  `screens/PartyMotion` handles input priority, locomotion and animation.
  It emits synchronous action cues without retaining the scene; turbo updates
  precede release cues and camera snapshots. Callbacks may not resize the party.
* `screens/PartyHud` owns status artwork, pickup cards, powerup selectors and
  localized help text. It is nonmovable because help borrows its message table.
  Scene rendering retains canvas/overlay order; HUD methods consume party
  snapshots without reaching back into PlayScene. Clear before releasing the
  borrowed glow texture supplied by Sumner's presentation.
* `world/PlayerFigure` owns costume selection, mesh/animation/voice archives,
  while `world/PlayerArsenal` owns active missiles and thrown-potion models.
  Bind the arsenal to borrowed level services after loading the shared weapon
  archive; clear it before releasing player figures and that archive. Neither
  owner may move while its model addresses are borrowed.
  `screens/PlayerAttacks` coordinates charge contacts, turbo strike effects,
  potion shields and projectile impacts against fixtures/opponents. It owns
  only transient attack state; the arsenal owns models and TurboMove owns the
  timeline. Preserve projectile -> strike -> shield phase order, and clear
  attack effects before releasing borrowed effect/weapon/figure resources.
  PlayerFigure owns
  node-name pose mapping and held/thrown weapon visuals. It has no scene or
  level dependency; the scene supplies actions, placement, lighting and alpha.
  Figure internals stay private. Borrowers of its missile model and effects
  must finish before it is released; the figure itself cannot move because
  its bound models and animator reference its own archives.
* `world/LevelSoundscape` owns level/common/ambient/narrator banks, music,
  scroll voice and target-opening sound lifetimes. It borrows only the audio
  output, not the scene or world. PlayScene supplies cues and listener snapshots.
  Named effects search level, common, then ambient; ambient items prefer TOWAMB.
  `stopCues` stops music/scroll/openings early in teardown; `close` also stops
  ambience and every voice it started before clearing banks: sequences borrow
  their clips, including queued narration. Other SoundPlayer clients are untouched.
  Close it after scene users finish and before destroying the borrowed SoundPlayer;
  do not move it while emitters borrow banks.
* `screens/BossVictoryPresentation` composes the `BossVictory` timeline with
  the wizard's borrowed model/animation, placement and typed captions. It
  consumes standing-party snapshots and messages, returning voice cues and a
  one-shot sparkle request; it never awards shards, plays audio or changes levels.
  `screens/BossSequence` applies rewards, legend impacts, audio and coin showers,
  using frame-local party snapshots; PlayScene chooses travel and camera/draw
  order. Clear the sequence before releasing audio, effects and item archives.
  Its completion signal is one-shot; its coin RNG survives level reopenings.
  Clear the presentation before releasing the level's item archive.
* `screens/LevelArrivalPresentation` owns the materialisation effects, their
  texture clock, the start camera and the sliding title. It takes party-position
  snapshots and borrows the weapons archive; clear it before releasing that
  archive. Animate before the world update, then advance its camera after the
  ambience/listener update. PlayScene keeps player entrance animation, world
  updates and the decision to start the welcome once arrival finishes. Missing
  art does not bypass the hold; the spawn effects expire independently of the
  camera ride.
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
  coordinate offset, alpha test, back-face culling, depth test/write); the Vulkan
  device sets cull mode, depth compare and depth write dynamically and pushes the offset and
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
* Characters: `PlayerFigure::costumeDirectory` picks the costume tier of ten
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
  Still to come for other levels: which portals a save has opened.
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
  them. Every trap hit over a point also stuns (`PlayerDeed::Spike` for
  spikes and blades, which play `SPIKEHIT`, `Reel` for the rest, `STUN1`):
  the victim stands where it was struck until the sequence ends and is not
  set reeling again meanwhile. Open question: a victim standing
  still is hit twice a cycle (once by the zero-frame ONA, once by ON), which
  the original's timings seem to do too but was not confirmed.
* `players/PlayerImpact` carries attack flags and travel direction through
  opponent contacts to `PlayerHealth`. Surviving hits select grounded reactions
  after guard/scaling gates: knockback (`HITREACT`), stun (`STUN1`), spikes
  (`SPIKEHIT`), or directional `FALLDOWN`/`FALLFRNT` followed by their get-up.
  Heavy flags are downgraded to knockback by a raised guard or shove; damage
  at most two removes knockback/heavy flags. Ordinary unflagged damage does
  not force a stagger. A lesser contact cannot erase a pending fall.
  Knockback translation, airborne/whirlwind physics and exact same-frame
  accumulation of damage/force remain separate unfinished behavior; grounded
  reaction selection is not a claim that these have been reconstructed.
* Level tuning: each level record of a realm's data wad holds seventeen
  floats from +0x9C (`LevelTuningRecord`, unpacked as `tuning`): the player
  level it is meant for, experience and damage multipliers, the difficulty,
  then enemy, generator and trap scales, where zero means "the difficulty".
  `WorldData`'s `LevelTuning` keeps what is used so far: `damage` (scales
  every hurt over a point), `trapRate` (times run at 1 / (rate * gain)) and
  `trapDamage` (times gain). The gain is the game's difficulty
  (`game.difficulty` in the settings: easy 0.667, normal 1, hard 1.5, the
  original's). G1 is 0.75 and 0.5, so its spikes do 10.
* Harm and death (`screens/PlayerHealth`, scene `blast`/`settleBlasts`): blasts hurt
  through `screens/LevelFixtures`, which owns chests, gates, traps, barrels,
  safe rocks, gas clouds and pending explosion chains. Its synchronous outputs
  delegate health/help/cards and opponent damage to the scene. Players are
  resolved before breakables and opponents for each blast; preserve that order.
  Clear fixtures before releasing their borrowed world/effect archives.
  `screens/LevelOpponents` owns enemy populations, generators, enemy missiles,
  boss meter, fractional reward accounts and following enemy effects. Its phase
  hooks retain the interleaving of blasts, legend rites, boss victories and
  player level changes. Clear shared effects before closing the opponent
  archives; the meter and missiles are released before their borrowed assets.
  Blasts hurt
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
  `turboC1` and `turboC2`. `players/TurboMove::begin` lines them up and
  `advance` emits each as the move's sequence reaches its `startFrame`: its
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
  `PlayerFigure::classFolder` gives them those of the class they shadow
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
* Combat ownership: `enemies/Combatant` is one noncopyable fighter borrowing stable
  `CombatantAssets`; it owns animation, movement, health/status and outgoing combat
  events, not a population or an encounter. `MoveDefinition.h` holds the shared
  target, move, attack, effect and meter definitions; `CombatEvents.h` carries
  neutral CombatBlow/Cue/Loss/Spew events. `CritterData` and `formats/CritterWad`
  retain their names only at the original CRITTER data boundary; file names and
  serialized records are unchanged. Execution is divided into CombatantMoves,
  Patterns, Motion, Attacks, Areas and View, with no per-kind id checks in those
  modules. Family definitions select policies instead of duplicating combat code.
  `CombatantBreath` and `CombatantProjectile` provide shared attack geometry and
  launch math; `world/CombatantProjectiles` owns the launched effects for both
  ordinary creatures and bosses, independently of their source actor's lifetime.
  `CombatantKind` names descriptor families (Golem 3, Boss 4, Gargoyle 7,
  General 8), distinct from level boss ids 34..44. Family definitions declare
  the expected kind; asset loading rejects a mismatched or unspecified family
  before binding models. Kind dispatch belongs at spawning/loading boundaries,
  not in shared attack execution. Keep raw serialized descriptor numbers intact.
  `Golem` owns realm-costume selection and five-unit knockback resistance;
  `General` owns its realm-costumed priority-move definition; `Gargoyle` owns
  form-specific assets and the defeated form used for key drops. These definitions
  do not invent missing patrol/statue behavior. `Critters` owns their shared
  sixteen-slot roster and shared assets, preserving stable ids, cross-family
  collision, slot-order updates and event submission order. Use its typed
  spawnGolem/spawnGeneral/spawnGargoyle entry points; the legacy kind-id adapter
  refuses bosses. `Bosses` owns one Combatant and its own assets, not a Critters
  pool; BossDefinition selects pattern scheduling and territory constraints.
  Keep encounter waking, legend staging, cameras and victory outside the common
  fighter. Texture clocks advance once per shared stock, not once per actor.
  Clear actors and external effect/projectile borrowers before releasing assets.
  `[combatant]` covers family policy, state isolation, shared capacity/collision,
  knockback, key drops, slot reuse, event order, independent boss ownership and
  retained asset/event lifetimes across boss replacement.
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
  to plays, else the loudest move whose target rule (`TargetCriteria`:
  distance window, a cone `minDot` wide pointing `yaw` from ahead, so TURN's
  points behind and the gargoyle's LEFT/RIGHT to the sides) and cooldown
  allow it: attacks (types from 128) over walks (52) over the stance (32);
  a move carries the body at its own `speed` a second (TYPE +0xAC is
  `roamRadius`, not speed) and turns it at its `turnRate`, never onto a player or
  another; over its harmful frames (`frameStart..frameEnd`, a second window
  too) its damage record strikes: a blow (0) whoever is within the part's
  radius plus its reach of the named node's posed position, a ring (3, the
  stomp) whoever is within reach of the feet, each player once a move.
  Breath (4) instead uses `enemies/CombatantBreath`: an animated-node segment,
  authored offset/yaw/pitch and min/max horizontal distance, tested against
  the player's expanded cylinder. Repeated contacts are routed by
  `LevelOpponents` through the recipient's shared quarter-second `breathGap`,
  for `damage` at the level's enemy damage scale.
  Projectile damage (1) is queued as `CombatShot` at the authored launch
  frame; move 133 repeats at `framePeriod`, including triggers crossed by a
  coarse update without re-firing a held frame. `CombatantProjectile` owns the
  retail launch math (0x8003d0a4 / 0x80030ae8): rate clamped to 0.5..1.5,
  speed interpolation factor 0.75, fixed-horizontal-speed ballistic aiming
  or normalized straight aiming, body/target selection, yaw spread and pitch.
  `world/CombatantProjectiles` owns moving effects, swept player contacts,
  world collisions, impact sound/visual and birth-to-loop-to-end transitions.
  Clear it before releasing the borrowed critter archives/tables. Launch
  policy (`behaviorFlags`, DAMG +2) is separate from player harm flags (+4);
  legend curbs use the former. Web shots with behavior 0x800 wait for the
  birth-to-loop transition before moving. Re-run `gdlunpack --only CRITTER`
  for the DAMG tail (morph lifetime, two morph indices and yaw spread).
  Targeted-area move 136 snapshots the selected player's centre at its first
  damage frame (retail `CritterCopyAnim` 0x8003c11c). Damage type 8 places
  its effects and impact at that saved world point, not the creature's
  active node; crossing frame zero must still emit the effect. The Genie's
  FOUNTAIN combines DAMG -100 and SFXX +97 with the player's centre,
  then uses the same snapshot for the later impact. Its basic radius contact
  is implemented; general effect-owned area lifetimes/status effects remain
  incomplete. Do not turn this snapshot into a homing target.
  Critter bodies apply `TextureAnimator` clock, sequence, then texture-node
  overrides at draw time, resetting shared model state between instances.
  The DJINN idle sequences pin the transparent GENIE_BEAM00 frame; BEAMARC
  selects the authored 30-frame texture cycle at rate 2. Do not hide the
  whole mesh or add a separate guessed beam lifetime.
  This is not complete boss fidelity: damaging impact areas/status effects,
  linked custom SFXX callbacks (including Dragon's fireball trail), generated
  stage hazards/minions, grabs (7), full attack interruptions
  and Chimera child-head control still need reconstruction.
  A hit takes the armour off (a point always through for a character), a
  block lets a quarter through and shrugs off the throw, and is worth
  amount / (1 + health) of the value to the hitter (a fiftieth less a level
  under the place's `playerLevel`, never under a tenth), paid in whole
  points as they add up; fifty taken and it roars; a floored hit plays KD
  (KB otherwise) and throws it (twenty, a golem five less); dead it plays
  DEATH, fades a second and is gone, a fifth of its value going to everyone.
  A gargoyle slain leaves the key its form is named by (`GARGEAGL`) where
  it fell. The bosses (`game/enemies/Bosses`) are their own thing, though
  the original keeps them in the critter pool as type 4: one to a level,
  named by kind (`bossNameOf(bossType)`, the dragon 34 to the garm 44;
  archive `MONSTERS/<NAME>`, tree prefix plus suffix, `LICH`), stood at the
  level's `boss` mark by `bossType`, asleep until the party comes within
  its table's `wakeThreshold` (or struck), fighting by the same move table
  through a `Combatant` fighter of its own (composition, not the shared
  pool: the step family, types 48 to 63, is chosen like the walk, and a
  named attack of speed, the lich's `CHARGE`, carries it), with a `BossView`
  (name, health, `fraction()`) for the meter through `PlayScene::bossView`,
  targets from 4000, and its worth paid the great ones' way. The legend
  items (`enemies/LegendItems`): each boss's is the item of its own realm
  (`legendRealmOf`: the chimera's scimitar 1, the dragon's ice axe 2, the
  genie's lamp 3, the spider's bellows 4, the temple's savior 5, the lich's
  book 7, the yeti's parchment 9, the wraith's lantern 10, the plague
  fiend's javelin 11; the underworld's and the garm have none), found as a
  subtype-13 pickup on the level whose record carries that `legend`
  (`level_masks[2]`/`[3]` are only the hints' record of the level beaten).
  `LegendWeakness` is the original's table (pmotion.c 2092, sfx.c 3225,
  gauntworld.c 1262): thrown, it takes a tenth of the boss's health now
  (a quarter of the lich's, 500 flat off the wraith, a head off the chimera:
  a third here until its heads are children), and freezes the dragon 1200
  ticks (`pausecnt` with the `SEETHROUGH` texture), blinds
  the genie 1800 and the plague fiend 18000 (`unkAC6`: no targets, turning
  at a tenth), or curbs the attacks whose damage entry has flag 0x4000
  (`unkAC8`: the spider's for good at 0.8 scale, tinted green in the
  original, the yeti's, wraith's and temple's for 29 s from its roar).
  `LegendRite` stages it as the original does (`lbl_8034489C` and the
  bearer's `quest_state`): `Bosses::bringLegend(player)` at the level's
  start for the first of the party with the item; the boss holds `READY`
  while it rises; risen, the bearer brandishes (the item is spent:
  `LegendCue::Brandished`) and throws a second on (`Thrown`: the toll and
  the weakness go on the fighter through `Combatant::freeze/blind/curb/
  resize`), the boss roars a second (chimera, lich, temple) or three after
  rising (`Roared`) and the curb wears off (`WornOff`); from the rise to
  the end of the roar the level goes dark by 0.8 through the ambient
  dimmer (`LegendRite::darkens`, gauntworld.c 1281's ambient special of
  -0.8 asked every tick in states 2 and 3), the boss alone drawn in the
  level's own light meanwhile. The rules are keyed by boss kind and realm,
  and the mountain's dragon fight (`LEVELB6`, scenario
  `level-b6-dragon.json`) runs the rite, the freeze and the coin spew in
  its scene test. Critter positions are floor anchors; drawing and posed hit
  points add the type's `floorOffset` (the Dragon's root is 18.5 units above
  the floor). The Dragon's damage and freeze begin when the axe lands, not
  when the throw gesture is requested. That impact ends its dimmed opening
  independently of the twenty-second freeze; other bosses still finish their
  opening on their roar. Its skin uses the level item's `SEETHROUGH` texture
  in keep-alpha mode (`texchangeidx = -4`): the original skin supplies the
  coverage mask (alpha above 2/255), the alternate texture supplies colour,
  and solid parts stay opaque instead of blending the ice alpha with the
  background. `DrawState::maskedTexture` uses the second texture stage,
  mutually exclusive with lightmaps. The skin blinks back to normal on timer
  bit 3 during the final 180 freeze ticks.
  `world/SafeRocks` draws the lair's six type-10/subtype-41 barriers from
  `SAFEROCK0L1` through `SAFEROCK3L1`; their tier and health fall under player
  attacks, rubble remains visible, and only standing tiers block movement.
  Selective breath obstruction is wired; type-6 arena attacks also reactivate
  cover after their eruption's wind-up. Boss-level item archives do not replace all realm assets:
  `LevelWorld::realmItems()` keeps the common archive when an own-level archive
  is selected. Traps prefer the boss-specific figure, then the realm's. The
  Dragon arena's ten `FLAMEV` figures come from `ITEMS/LEVELB`, not `LEVELB6`;
  unpack the former too (`gdlunpack <assets> <out> --only levelB`). Their
  OFF/ONA/ON/ONB sequences now supply the hazard timing. The authored particle
  node still needs sequence-aware emission in `ItemFigure`; do not start it
  continuously during OFF. `screens/LegendPresentation` owns the held/flight/charge
  effects, gesture retry and flight sound lifetime. `PlayScene` supplies
  bearer/target snapshots and applies returned impact events to `Bosses`;
  the presentation never mutates gameplay actors. Clear it before releasing
  its borrowed archives or effect store. The ice skin is a draw-time input,
  not a texture retained in the fighter's simulation state. `LegendShow` is
  how it looks (pmotion.c 2092-2447, sounds_evt.c `fn_8009C9DC`): the
  `LEGENDHLD` of the boss level's own item archive glows in the hand for
  bosses 34-39 (`SfxSetParent` on `hand_node`) and 8 over the head for
  the rest until let go of (999999 s); the bearer's gesture is
  `ATTPWRATHROW` (anim 99: 34/35/38/39, `PlayerDeed::ThrowLegend`),
  `SSHOT1` (107: 36/37, `ShootLegend`) or `MAGICS` (115: the rest,
  `HurlLegend`), the release at its action bit (`legendReleased`, no
  potion or weapon going with it); `LEGENDPRJ` then flies at the boss at
  20 a second for at most 6 s from 2 up (34/35/36/38, `Flight::Flies`,
  homing on `hitnode` in the original; landed, `LEGENDFX` bursts there),
  rides 5 ahead of the bearer (37, `WithBearer`, 3 s), or is set on the
  boss (`AtBoss`: 41 for 5 s, 40/42 for 30 with their offsets, the yeti's
  as `LEGENDFX` then `LEGENDFX2`), the tree named by `then` taking over
  as its sequence ends (`SfxSetMorph`). Sounds: `S_LEGWPUP` (common 100)
  brandished, the realm's `S_<L>LEGWTHROW` thrown (J's is `S_JEGWTHROW`),
  `LEGWFLY`/`LEGWALL` let go of (stopped on landing or wearing off),
  `LEGWHIT`/`LEGWALSTP` landed, `LEGWPDN` worn off; `soundNamesOf` gives
  the spellings to try and `LegendPresentation` selects the first playable
  name through the scene's audio port (level, common, then ambient bank).
  Held and flying items are full-bright and do not write depth. The
  charge starts `COMBO_SPH` tinted by costume colour and that colour's combo
  burst with a 0.333 animation time scale. `EffectTrees` passes the camera to
  billboard nodes and draws code-created trails from moving effect roots.
  The projectile trail (34/35/38) emits thirty particles a second for three
  seconds, at one unit a second, width two, with a one-second life and
  one-second alpha fade; the axe uses STATIC's `PARTICLE1_A`. Not yet: the
  bearer's own glow (`MBTreeSetAmbientAdd 0x1FF`), the charge's dynamic light,
  the fade of the set
  effect's last seconds (gauntworld.c 1333), the spider's `0xFF40FF40`
  tint, the genie's `LEGEND1` for 28 s, the
  in-world bar (`typeFlags & 0x800`, the `GMETER` tree hung at the type's
  `healthBarOffset`),
  the patterns (PTRN), phases, cameras, children (the chimera's heads),
  projectile moves, the general's waypoint patrol, the gargoyle's
  fireball, per-part damage and breaking, the critters' sounds, the
  statue's waking, and a boss level unpacked (`--only levelG5`).
  Dragon breath's collision and effect use the active animated node. The
  unflagged FIRE move cue attaches there at `sfxFrame` (38 for BREATH), not
  the later damage frame (49). `EffectTrees::placeAt` preserves its full
  basis. `engine/world/TreeParticles` plays the archive's particle nodes,
  including particle-only effect trees: FIRE has two emitters and no mesh.
  Templates govern their direction, emission/fade, speed, size and textures;
  shared texture-animation frame replacements preserve live particles.
  Still incomplete: retail's item obstruction/filtering (including the
  safe rocks), and auditing per-particle texture selection against retail.
  Do not substitute all collision obstacles for that selective item query.
  All move effects now start with their sound at `sfxFrame`/`sfx2Frame`
  (`CritterAnimate`, 0x8003b300), once per move even when an update crosses
  the frame. The effect's animation already contains the visual wind-up;
  do not add another delay until the damage frame. SFXX flag 1 attaches to
  the full elevated model root (not floor position), an unflagged move effect
  uses the active animated node, and 0x40 snapshots a world-space placement.
  Flag 0x80 without 0x801 uses the initial geometry base with a world-axis
  offset. Root/node parents carry scale once and follow rotation as well as
  translation; `LevelOpponents` updates them before effects are rendered.
  This follows `CritterSfx`/its create helper at 0x8003d7e0/0x8003dc64.
  SFXX's parent-of-root/global overrides and custom callbacks still require
  reconstruction, as do camera-shake cues. The Lich's empty SFXX flag-0x20
  row calls the arena callback (0x80063c58) to HIDE `G5BIGDIRT`, not animate
  it: world lookup 0x800a9c50 supplies its tree, and 0x800ba368 sets flag 1
  with recursion disabled. The draw traversal (0x800c7a70) skips that mesh,
  not its children. `Combatant` retains nonvisual cues, `LevelOpponents`
  dispatches them, and `LevelWorld` keeps this stage mesh separately
  controllable rather than baking it into shared geometry. Garm's matching
  callback hides `H4NSFFXL_PURPLE`. Visibility leaves alpha, transforms and
  collision unchanged and resets on level load. The Skorne1 callback's
  counter at 0x80344958 still needs its consuming behavior reconstructed;
  do not treat it as a hide/animation request or claim all callbacks complete.
  Scenarios: `level-g1-general.json`.
* Boss locomotion has two encounter roles; neither role implies melee-only
  attacks. Dragon, Plague Fiend, Yeti, Wraith, Chimera and Genie are anchored;
  Lich and Spider Queen pursue the player. Preserve the actual TYPE/MOVE data
  instead of implementing one generic chase AI or forcing every anchored boss's
  authored step to zero. `CritterMovement` follows `CritterTranslate`
  (GC 0x8003a9c4) and `CritterRotate` (0x8003af4c): TYPE +0xAC limits horizontal
  distance from home, MOVE +0x84 is pace, and TYPE +0xCC limits facing relative
  to spawn yaw. TYPE flag 0x20 selects square rather than circular territory;
  0x40 uses initial facing for steps and 0x400 removes the facing limit. Explicit
  `defaultPos` supplies home unless its Y is the 999 sentinel. Old unpacked
  JSON calls the radius `speed`; the loader accepts it, but new exports say
  `roamRadius`. Never reinterpret this value as a cap on movement speed.

  | Boss | Locomotion / home radius | Authored attack families |
  |---|---|---|
  | Dragon | Anchored, 3; fixed facing | Near claws; breath, fireballs, stomp and wing attacks |
  | Plague Fiend | Anchored, 6; local lateral steps | Bite; acid, sprays, gas and splash |
  | Yeti | Anchored, 5; destination-driven local steps | Claws/swipes/grab; breath, rocks, stomps and pounds |
  | Wraith | Anchored, 3 | Near swipes/blender; stretch, thrown blades, snakes and bolts |
  | Chimera | Anchored, 12; entrance and local repositioning | Separate head move tables, plus root SUPER attacks |
  | Genie | Anchored, 0; turns in place | Near claws; beams/sweep, wind and targeted rock fountain |
  | Lich | Pursuing, 25; advance, retreat, sidestep and charge | Axe/spike/grab at close range; head toss, spit and hand attack |
  | Spider Queen (`DRIDER`) | Pursuing, 22; advance, retreat, scurry and charge | Close whip/kick attacks; web, egg, spit and spider projectiles |

  MOVE target distance, bearing and vertical windows govern move selection;
  DAMG and harmful animation frames govern contact. Do not use a shared melee
  distance or allow proximity-independent melee just because a boss is anchored.
  Direction types 50/51/53 mean left/right/backward, not forward pursuit.
  Type 56 uses the ready-move search's player destination (retail +0x1FC),
  refreshed during locomotion and retained if the target disappears. Boss steps
  stay inside TYPE.roamRadius; they are not unrestricted pursuit. CritterInitHeader
  derives the ready-search flag 0x10000 from MOVE types 48..57, so the raw TYPE
  flags alone do not establish that stepping is disabled. Non-player waypoint
  producers and Chimera child-head logic remain separate reconstruction work.
  `[boss-movement]` tests cover bounds, facing, direction,
  legacy/new export keys, synthetic near/far encounters and the eight retail tables.
* Boss attack selection (`enemies/CombatantPatterns.cpp`) consumes PTRN
  sequences and MOVE/PTRN health gates. Re-run `gdlunpack <assets> <out>
  --only CRITTER` after updating: older manifests omitted patterns and now
  produce a warning instead of silently losing combos. Rate scale is
  `0.5 + 4.5 * (1 - health / (1 + maxHealth))`; projectile speed uses its
  existing clamped interpolation. A maximum at or below the minimum means
  no upper bound. The exported `idleGate` is a home-distance limit, not time.
  Patterns gate entry, then continue their authored indices (including repeated
  moves); individual move gates do not cancel a chain. Pattern and solo-use
  timestamps are independent. Unused attacks are immediately available on a
  fresh scenario, avoiding an artificial cooldown from resetting its clock.
  Selection uses last-use order and MOVE interrupt policy, excluding linked-only
  attacks and invalid required nodes. This is not yet the full mid-animation
  interruption/child-head scheduler. `[boss-attacks]` tests exercise synthetic
  chains, phase boundaries, target eligibility and the three retail WADs.
  Crossed contact/death frames survive coarse updates; death holds use MOVE.hold,
  fading over the final half second. Safe rocks explicitly occlude breath before
  player damage/cooldown; other eligible retail obstacle families still need the
  same selective cover query. Genie blindness begins on lamp impact, with the
  level's LEGENDFX attached at root+(0,6,0) for 28 seconds. Its flight and attachment
  have independent lifetimes, and losing the bearer does not detach the effect.
  Move-effect node lookup falls back to the animation root, separately from the
  body-origin fallback used for targeting/hit positions. Lich START's GENFX has
  no node and zero offset: adding TYPE.originOffset incorrectly raised the gravel
  ten units. Synthetic attachment tests and the retail START cue cover this.
  Remaining combat work includes Lich grabs/sticky hands, whirlwind player motion,
  effect-spawned BOSSGEN instances, effect-owned impact damage/trails, SFXX camera
  shakes and the Lich's arena-dirt visibility cue. Playing a move or its visual
  is not evidence these gameplay paths are implemented.
* Spider Queen and Wraith's type-2 attacks use `CritterArea` and
  `CombatantAreas`: constant-radius root-attached sectors, independent of
  the move's remaining frames and of whether their SFXX has visible artwork.
  Eight DRIDER and three WRAITH damage records use this path. `NULLFX` is an
  intentional invisible damage carrier, not an absent attack. SFXX.life wins;
  otherwise use its sequence duration, with NULLFX's zero-frame/rate-30 sequence
  lasting one second (the original StartFXTree fallback). DAMG and SFXX offsets
  compose before local yaw/pitch. Radius comes from maxDistance, scaled once by
  the creature; neither DAMG.radius nor the body's targeting-origin offset is
  the damage sector's extent/origin. ProcessEffects (0x80094be0) uses a horizontal
  cone with an 85% dot threshold inside 30% of the combined radius and checks
  height separately. Contacts share `PlayerRuntime::effectGap`, capped to one
  second or the effect's remaining lifetime, not a once-per-move hit list.
  Safe-rock obstruction is queried only beyond ten units, with a 0.1-unit probe.
  This covers the shipped stationary root policies, not arbitrary moving/morphing
  areas, expanding rings, webs, or all effect/projectile cooldown interactions.
  Unsupported area parenting/motion policies warn rather than becoming guessed
  radial damage. `[boss-areas]` covers synthetic physics/lifetimes, all eleven
  exported records and both actual encounters. The new `spider` and `wraith`
  scenarios require LEVELD5/LEVELJ5 and their own item archives to be unpacked.
  Unpack realm items LEVELD/LEVELJ too: `LevelWorld` lends boss-specific textures
  first, then realm textures, to world geometry, texture animations and particles.
  LEVELD5's torch particles reference LEVELD's P_TORCH, absent from its own item
  archive; `[boss-arena]` verifies the borrowed texture rather than a white fallback.
* Wraith attack coverage (`[wraith]`) exercises all eleven attack families across
  health/range windows and all four projectile damage records. Blank attack-node
  names use the model root, not TYPE.originOffset: J5 snakes launch at root+13,
  not ten units above that. SFXX flag 0x40000 owns a move-lifetime effect;
  CritterCopyAnim (0x8003c11c) cancels it on the next successful move change.
  Wraith START's long-lived INITFX must end before START2's GENFX/GENFX2.
  Projectile DMG_SUPER passes through players, with the shared player effect-gap
  gating repeat damage. DMG_SUPER+DMG_REFLECT spends its pass-through on contact
  and leaves an impact; ordinary snakes retain it through SNAKEFX -> SNAKELOOP.
  The lantern caps the birth effect at 0.25 seconds, not the snake's 15-second
  morph lifetime. THROW's sticky ATK07LP impact becomes ATK07WEB for twenty
  seconds, floor-aligned by SFXX 0x10, with stationary radius-two contacts.
  Sticky contacts run at the authored 30 Hz and request WEBREACT rather than an
  invented stun or knockdown. Tests cover 30/60/120 Hz, expiry, archive cleanup,
  entrance routing and actual WRAITH artwork. `python scripts/scenario.py wraith`
  starts at the level entrance, outside wake range; `wraith-attacks` starts close
  enough to wake it. Need LEVELJ5, LEVELJ and MONSTERS/WRAITH unpacked.
  This is not a claim of completed visual/AI parity: the common scheduler's
  mid-animation interruption policies, sleeping INIT staging, camera arithmetic,
  and every shield/reflection interaction still need retail comparison.
* Chimera SUPER's type-3 NULLFX now uses the expanding `CritterArea` path rather
  than an immediate omnidirectional full-radius hit. In ProcessEffects (0x80094be0),
  phase is remaining/lifetime: radius is maxDistance*(1.33-phase), damage is
  baseDamage*1.5*(phase-0.33), and both stop at phase <= 0.33. The directional
  cone still applies, and hit immunity lasts remaining+0.066667 seconds (subject
  to the existing damage threshold/half-second override). Stationary attached
  type-3 effects with supported policies share this path; unsupported policies
  retain the older approximation and are not validated retail behavior.
  Unflagged attachments follow the move's animated node, while SFXX 0x800 fire
  effects (SFIRE1/2) follow the full body transform, including rotated offsets.
  `[chimera]` covers the curve, synthetic execution without assets, and actual
  SUPER/fire cues. `python scripts/scenario.py chimera` loads A5; unpack LEVELA5
  (world and items), LEVELA (realm textures), and MONSTERS/CHIMERA first.
  **Chimera is not behavior-complete:** the current actor loads TYPE row zero
  only. Retail CritterNewInst (0x8003e048) walks children EAGLE/LION/SNAKE,
  sharing CHIM's tree but detaching BODY1_EAGLE/LION/SNAKE animation subtrees.
  Each has 15 local moves, its own health/collision nodes and damage bindings.
  CritterBossAI (0x80039ad8) synchronizes them with body patterns or body SYNC;
  outside those windows they copy the body's sequence/frame. Independent
  unsynchronized actors are not an equivalent implementation. Head hit routing,
  death/stumps, legend-item head removal and type-56 repositioning remain open.
  A5 also has three tier-1 SAFEROCK instances and only SAFEROCK1L1 artwork.
  The current loader warns about absent optional tiers 0/2/3; those warnings
  do not mean the initial tier-1 cover mesh is missing. Do not copy Dragon's
  three-tier rock artwork into this arena to suppress the warnings.
* Plague Fiend SPOUT uses DAMG type 5, not an attack at the boss's feet or a
  player-targeted fountain. Retail dispatcher 0x8003ca98 creates one effect per
  collected subtype-41 stage item and reparents it to that item's node. K5 has
  three such points. `SafeRocks::attackAnchors` supplies their static transforms
  (including destroyed cover, up to the original sixteen); the scene passes
  them through LevelOpponents/Bosses to Combatant without introducing item or
  scene dependencies into attack execution. Both authored damage windows fire
  once: ATCK10FX at frame 8, then the separate NULLFX damage at frame 20.
  Each uses its own lifetime and expanding damage curve. World-placed cues keep
  the stage orientation and do not follow boss movement; LevelOpponents retains
  and stops their effect handles before releasing borrowed boss artwork.
  `[plague]` covers asset-free execution, actual PBOSS attacks and all three
  rendered K5 placements. `python scripts/scenario.py plague` needs LEVELK5,
  LEVELK and MONSTERS/PBOSS unpacked. This is not a completed fidelity audit:
  SPLASH still takes the old approximation because its damage carrier moves
  at speed 1; acid impact damage/status behavior and legendary-javelin timing
  need further investigation. K5's BLOB_BOSS texture animation also references
  BLOB_BOSS00 in MONSTERS/PBOSS, outside the current world texture lenders;
  the missing-frame warning is still open, not evidence the texture is absent
  from the retail assets. Moving stage anchors are not implemented by this static
  type-5 path; type-6 reactivation uses the separate targeted eruption path below.
* Yeti POUND/POUND2 uses DAMG type 6. `CritterDoDamage` (0x8003ca98) selects an
  inactive stage rock nearest its target in the horizontal plane, starts
  ATTACK12_S0 there and activates the obstacle after (effect frames - 1)/30
  seconds. `SafeRockNearestTarget` (0x80035bc8) uses the piecewise `fqdist`
  metric (0x800bcb44), not Euclidean distance; close ties can select differently.
  Its eight slope constants are retained in the targeted arena selector.
  `Combatant` receives the full anchor roster with active flags and emits activation requests;
  `SafeRocks` owns dormant/solid state and delayed activation through
  `LevelFixtures`. No scene/item dependency is introduced into combat execution.
  Encounter setup detects referenced type-6 damage and initially hides its rocks,
  following `CollectSafeRocks` (0x80063f10). I5 has thirteen authored rocks, eight
  enabled for one player. They begin invisible/non-solid, not as visible rubble.
  POUND's 36-frame effect deals expanding area damage at its fixed stage anchor;
  the selected rock becomes visible/full-health (90) after 35/30 seconds. Destroyed
  rocks become candidates again. Pending eruptions do not mark a rock active and
  a later eruption may reset its timer. Contacts do not migrate with the player.
  `[yeti]` covers synthetic selection/coarse frames/lifetime, actual YETI moves,
  I5 artwork/collision, party gating and scene event routing. Detached move cues
  and projectile impact/end effects (including ATTACK8FXC) are retained for
  cleanup before their borrowed archives are released, just like attached cues.
  `python scripts/scenario.py yeti` runs I5; unpack LEVELI5, LEVELI and
  MONSTERS/YETI first. Without a player target, the selector uses its random
  initial cursor and retail's asymmetric index scan: it checks entry i but
  returns (cursor+i+1)%count, which can select an already-active rock.
  Yeti grabs test JOINT_13 during frames 25..30, carry the player's full body
  transform through the hand animation, and release at frame 100. DAMG's force
  is 1000 along normalized (forward.x,-0.1,forward.z); its 100 damage is deferred
  until landing. `PlayerCapture` owns held/thrown state, with input suppressed,
  GRABBED/FALLDOWN/GETUP animation, cancellation on owner loss/death/interruption,
  horizontal speed capped at 40, descent 16 and velocity decay 0.9 per 30 Hz frame.
  Shared collision still uses the port's floor/wall solver, not an assertion of
  identical trajectory on every retail surface. SFXX shake cues perturb camera
  attention by radius 0.1 for 90 ticks without accumulating into its base pose.
  STOMP's ATTACK4FX has root-parent SFXX flags: its (0,-3,0) offset REPLACES
  DAMG's (0,8,-25), rather than adding to it. Using both shifts the expanding
  damage backward out of reach of players in the front arena. Tests require a
  hit at distance 60 using actual YETI data. Iceballs carry DMG_REFLECT (0x200000):
  world contact reflects velocity, scales upward velocity by 0.4 and caps lifetime
  at ten seconds (otherwise subtracts one). World contact uses half the player-hit
  radius. Floor-object flag 0x4 raises the impact to the surface plus two units;
  preserve this flag through collision loading and moving-object transforms.
  Without the lift, shallow throws consume their lifetime in repeated micro-bounces.
  They must not burst on first floor contact. Their maxDistance is zero,
  so there is no extra splash-damage area; ATTACK8FXC's SFXX flags are zero, so no
  floor-aligned impact policy is requested. `[yeti]` includes actual animation,
  stomp reach, floor-bounce and an actual I5 hand-launch/player-contact regression;
  ATTACK4FX's mesh flag 0x40 requests always-pass depth compare, separately from
  0x80 disabling depth writes. Ignoring it lets the arena floor hide the stomp.
  Texture-node overrides are subtree-local: frame substitutions retain their
  texture-slot filter, but UV scale/add applies to every material below the node.
  ATTACK11FXB's scroll record names slot 0 while its trail mesh uses slot 116;
  slot-filtering the UV transform leaves the grab streak permanently visible.
  ATTACK3FX has eighteen branches sharing slot 11 with separate delays; applying
  the last branch globally erases the authored frost-breath progression. Initialize
  texture modifiers at frame zero as well as on subsequent updates.
  Synthetic wall
  reflections/lifetime and grab/throw tests also run without game assets.
* The boss's health meter (`screens/BossMeter`, bound in `bindEnemies` from
  `Bosses::meter()` and the boss's own archive, drawn over the status boxes)
  is the original's HUD meter (`HealthMeterStart/Update`, boss.c 471-585):
  the type record's `meterPieces` strips of 256 (two: the whole virtual
  width) at y 8, each `METER_BG<n>` (when `typeFlags & 8`) under
  `METER_FG<n>`, at the original's blit alpha 112 (opacity 143 here); the
  fill is cropped, not squeezed: the first strip's runs from its
  `meterLeftInset` cap to its end over the first half of the health, the
  second's from its start to `256 - meterRightInset` over the rest (one strip
  runs cap to tail); the health shown eases at three a tick; the backgrounds
  are tinted `0x8080FF` while the boss is frozen; it goes with the boss's
  death. The type fields (`CritterWad` 0xD0 `healthBarOffset`, 0xF8..0xFE
  the meter's pieces/advance/insets) need `gdlunpack --only CRITTER` again.
* A boss fight's camera is `world/BossCamera`, from the realm data's BCAM
  records (`BossCameraInfo` on the boss level's `LevelInfo`, index
  `bossCameraIndex` @0x8C of the level record; G5: distances 25..75, player
  distances 25..30, pitch 0.31..0.44, attention offsets (0, 2.3, -2) near to
  (0, -9.3, -2) far, maxYaw pi): while the boss (or, after it, the wizard)
  stands it looks from behind the party along their line to the boss, swung
  no further about the boss's facing than `maxYaw` when the party is round
  it (recomputing `cos(maxYaw)` rather than using the file's stale cache),
  at the record's pitch (steeper nearer), backing off by
  the original's steps (10 out when something is cut off, 2 x (2.5 -
  margin) when within 2 of the edge, in by (margin - 2.5) past 4) to keep
  the boss's base and body centre and every player in view.
  BCAM bit 0 selects the live root plus vertical drift; without it the
  attention anchor is the base root saved by geometry initialization
  (`CritterInitGeo` 0x8003e3e8), and bit 4 selects the party. The Genie's
  flags 2 therefore exclude its ten-unit vertical drift.
  The original's
  `BossCamBossCalc` is only partly reconstructed (bosscam.c); this is its
  described behaviour, not its arithmetic.
* The great ones' sounds and effects: each critter's SFXX records are read
  whole (`CombatEffectDefinition`: tree, `%c` sound format, offset, life, scale,
  flags, link) and set off as `CombatCue`s: a move's at its `sfxFrame`
  (its own effect sequence supplies the wind-up), a strike's
  (`AttackDefinition::sound`) at the part it strikes
  with, a hit's mark (`hitSoundClose` @0xF4 for a blow, `hitSoundFar` @0xF6
  for a missile) where it landed (`EnemyHit::where/close`). The scene plays
  the trees from the creature's archive or the weapons' (`HITDIE`), the
  sounds from the level's bank (a boss level's is the boss's own).
* The end of a boss fight (`screens/BossVictory`, the original's
  `DoGoodWizard`, auxscreen.c 236): the fall gives everyone the realm's
  shard (`Relics::shards`, bit `LevelRef::orderOf(realm)`: the tower's order
  13, 7, 2, 1, 11, 4, 3, 9, 10, 5, 6, 8), the `BOSSKEY` tree of the level's
  own item archive (`LevelRef::ownItems`, `ITEMS/LEVELG5`, loaded over the
  realm's when present; it also holds `WIZARD` and the `LEGEND*` effects)
  rises where it fell (30 s, then `BOSSKEY2`) with `S_BOSSKEY<letter>`;
  five seconds on (ten for the demon and the garm) the wizard fades in
  (4/255 a tick) three units over the middle of the boss mark and the party,
  and says, typed a character every two ticks with a second between pages
  under the view, the boss's `<NAME>_SPEECH` with `S_DEFEATVOX<letter>`,
  then `RUNE_PHRASE0/1/1B/2` with `S_RUNEVOX0/1/2<letter>` by how many of
  the realm's runestones (the levels' `rune` records) the party holds; two
  seconds later (ten while gold lies untaken, `BossVictory::setGoldLeft`
  from `LevelWorld::goldLeft`, cut to two once it is all gathered) the
  party sparkles (the spawn effect) and, 35 ticks on, travels to the tower.
  The coins (`enemies/BossCoins`, the original's `BossSpewCoins`, boss.c
  262): the death move's first damage record is of type 9 (`AttackDefinition::
  kSpew`: its `yaw`/`pitch` turn and tip the body's facing, `minSpeed`
  @0x30 is the throw's speed, `acos(minDot)` half its arc), and when the
  death reaches its frame (the lich's 95th) the fighter reports a
  `CombatSpew`; the scene then throws, for each player in the game, the
  realm's counts of bronze (500), silver (1000) and gold (5000) coins
  (`kCounts`, the original's table at 0x801189E0: the town's 4/1/0, the
  castle's 2/1/1...) fanned evenly over the arc at 0.85-0.95, 0.8-0.9 and
  0.75-0.85 of the throw, as the level's `COIN_BRONZE/SILVER/GOLD` items
  (`PlacedItems::throwItem`: gravity 32, bounce 0.4 until a bounce would
  not clear the touching-down height, sideways drag 0.5/s aloft and 4/s
  on the floor, lost when falling with no floor under them, untakeable for
  two seconds), and its blast (1000 over 1000 units) takes the swarm and
  generators with it. Not yet: the demon's four relics (`BGNTR_IC` and
  so on), the demon's rune-yes/no speech, the caption's original font
  placement. Scenario: `level-g5-lich.json`. Critter data unpacked before
  the spew's speed was read (`--only CRITTER`) throws nothing.
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
  the gold and health. Scenarios take `gold`, `health`, `keys`, `potions` and
  `legends` (the realms whose bosses' items are carried) per member and
  `items` (`name`, `position`); `tests/scenarios/tower-items.json`
  lays a spread out. Powerup timers do not run in the tower (nor did the
  original's).
* The rest of the pickups (`players/Relics`, in each `ClassProgress`, saved
  under `relics`): subtype 10 is a runestone (its record's `value` the rune,
  0 to 12; `RUNESTONE` card, `S_PICKUPRUNE`; refused with `ALREADYHAVERUNE`,
  help 90, when held; the scene's `shareRune` gives it to everyone in play and
  the narrator counts the party's, `S_RUNEFOUND1` then `S_RUNE2`..`12`; the
  `GETRUNE` burst of the powerups archive plays), 13 a legend item (its value
  the realm whose boss it is for; `LEGEND` card; named by help 113 + realm,
  `LEGEND_ITEMS000`..`010` in the strings, voiced from `VOICE2`, the
  narrator's second bank; `Relics::legends` is the original's per-class
  `rune_near` mask, set by the pickup), 14 a scroll (its instance's first
  parameter is the page, from one, of the level's `SCROLLS<level>` message
  in `scroll_e.json`: read on the spot through `openMessage`, nothing kept;
  `ItemTaking::Outcome::Shown`), 15 a crystal (the scene's own path), 16 a
  gargoyle piece (value 0 the serpent's, 1 the eagle's, 2 the lion's,
  counted up to 12/20/28, the original's `completion1` records; `GOLDNICON`
  card, `GETGARG` burst). Not pickups: `BOSSKEY` (subtype 11, the boss's
  death effect) and `TIMEBOMB` (44, a hazard like the barrels). The legend
  levels (`LevelInfo::legend`: D4 1, A2 2, I4 3, C3 4, E1 5, K4 7, G3 9, J2
  10, B5 11) are not unpacked yet, so no legend item is placed.
* Using what is carried: `PlayBindings` adds `usePotion` (E, pad B),
  `throwPotion` (Q, pad X) and the selector's four presses (I/K/J/L, the
  pad's directional buttons, which therefore no longer walk by default: the
  stick does, as in the original). `PlayerDeed` tells `PlayerAnimator` what
  the buttons ask; a potion plays `MAGICS` then `MAGICR` (used) or
  `THROWPOTIONS` then `THROWPOTIONR` (thrown), the release's start being
  `potionUsed()`/`potionThrown()`, one potion a press; the legend deeds
  (`HurlLegend`/`ThrowLegend`/`ShootLegend`) play the same `MAGICS`, the
  strong throw's `ATTPWRATHROW` or `SSHOT1`/`SSHOTR` with `castingLegend()`
  on and `legendReleased()` at the moment of release, and set none of the
  potion or weapon flags. `PlayScene` takes the
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
* `screens/LevelMessages` owns the level's localized scroll text, scroll
  presentation and bitmap font/painter shared by play overlays. It borrows
  STATIC textures, cannot move (the scroll references its own painter), and
  returns dismissal audio cues without playing sound. Clear its painter's
  users before clearing it, then release the static textures. PlayScene keeps
  input eligibility, pause priority and welcome-camera progression.
* `screens/SumnerVisit` owns hint artwork, greeting/visit timing, the scroll's
  input owner and localized hint answers. It borrows artwork and the text
  painter; clear it before their owners release them. It cannot move because
  its menu points at its own arrow sprite. PlayScene retains proximity checks,
  owner-input routing, audio and Sumner's gestures. The greeting countdown
  continues while the spot is empty; its current visitor receives the scroll.
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
  `python scripts/scenario.py genie` launches a full name, unique suffix or
  JSON path in the existing Release build. `--list` lists scenarios, `--build`
  builds first through build.py, `--preset` selects a build, and `--frames`
  bounds a smoke run. Additional game arguments follow `--`.
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
  Texture animations come in two kinds (the record's `flag`): free-running
  ones (`-1`) step on the game clock, as `TextureAnimator::step` does;
  the rest are keyed to a sequence's frame and never step: a texture node
  (type 3; `TreeNodeInfo::textureAnimation`, its data offset from the
  tree's sequence table into the archive's animation list) or a
  sequence's own run (`TreeSequenceInfo::textureAnimationStart/Count`, an
  index and count into that list) is read off with
  `TextureAnimator::motionAt(info, frame)`: a cycle counts from the
  record's `offset` frame, a frame every `rate`, and holds its last; a
  scroll runs `scrollStateAt` (the original's `CalcTexScroll`: the slide
  eases over `rate` frames, runs steady to `frames`, then holds; the
  stretch it puts on the coordinate it slides is what it reaches less the
  slide: nought before it starts, which collapses the picture and hides
  the glow until its frame, up to `frames / rate` at the end).
  `TextureMotion::scale` reaches the draw as `DrawState::uvScale`
  (`TreeModel::setTextureOffset(slot, offset, scale)`; the shaders' push
  constants carry it after `params`). `EffectTrees` applies them at the
  effect's frame, which is how the lich's axe glow scrolls only from its
  29th frame and the stomp's ring cycles from its 32nd. Players and
  critters do not read theirs yet.
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

* Use the numeric aliases from `engine/core/Types.h`: `u8`, `u16`, `u32`,
  `u64`, `s8`, `s16`, `s32`, `s64`, `f32`, `f64`, and `usize` (container sizes).
  Include that header directly where its aliases are used. They are aliases
  of the standard fixed-width integers, `float`, `double`, and `std::size_t`,
  not wrapper types. Keep native spellings where an external API or entry
  point warrants them. Never replace a 64-bit field with `long` (its width
  differs on Windows and Linux). Ownership macros live in `SpecialMembers.h`.
  Asset, serialized, audio and GPU field widths must remain unchanged.
  The core type tests enforce the supported platforms' 32-bit `int` and
  `unsigned int` interoperability with fixed-width interfaces in both CI builds.
  When a cast supplies an initializer's type, use `auto` instead of repeating
  it; CI's clang-tidy checks this as well as the editor.

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
  For responsibility-only PlayScene refactors, use an incremental build,
  tests for the extracted component and affected scene behavior, and lint/editor
  checks limited to changed files. Add or update regression tests for the new
  boundary. Do not rerun project-wide tests or lint for each extraction;
  reserve broader validation for changes with broader impact.
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
