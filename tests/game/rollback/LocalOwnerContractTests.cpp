#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/MessageTable.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/LocalRoster.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/SampleLevel.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "FakeMechanic.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/audio/LevelSoundscape.h"
#include "game/audio/VoiceQueue.h"
#include "game/bosses/BossSequence.h"
#include "game/bosses/BossVictoryPresentation.h"
#include "game/bosses/Bosses.h"
#include "game/bosses/LegendItems.h"
#include "game/bosses/LegendPresentation.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Party.h"
#include "game/characters/Progression.h"
#include "game/characters/Relics.h"
#include "game/config/GameConfig.h"
#include "game/context/GameServices.h"
#include "game/critters/Combatant.h"
#include "game/critters/CombatantFixture.h"
#include "game/critters/CombatantKind.h"
#include "game/critters/CombatantProjectile.h"
#include "game/critters/CombatantProjectiles.h"
#include "game/critters/CritterData.h"
#include "game/critters/CritterStatues.h"
#include "game/critters/Critters.h"
#include "game/effects/EffectTrees.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/EnemyMind.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/Generators.h"
#include "game/enemies/LevelOpponents.h"
#include "game/enemies/PlacedEnemies.h"
#include "game/hud/BossMeter.h"
#include "game/hud/ChallengeHud.h"
#include "game/hud/HealthMeter.h"
#include "game/hud/HelpMessages.h"
#include "game/hud/LevelMessages.h"
#include "game/hud/PartyHud.h"
#include "game/level/Breakables.h"
#include "game/level/Chests.h"
#include "game/level/DestructibleWalls.h"
#include "game/level/ExitPortals.h"
#include "game/level/FallingScenery.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelFixtures.h"
#include "game/level/LevelTransporters.h"
#include "game/level/LevelTriggers.h"
#include "game/level/LevelWorld.h"
#include "game/level/LockedGates.h"
#include "game/level/NativeStateResources.h"
#include "game/level/PlacedItems.h"
#include "game/level/Rotators.h"
#include "game/level/Rubble.h"
#include "game/level/SafeRocks.h"
#include "game/level/ScriptedFixtures.h"
#include "game/level/SkorneArena.h"
#include "game/level/SpawnedObjects.h"
#include "game/level/StageRegistry.h"
#include "game/level/StaticScenery.h"
#include "game/level/Traps.h"
#include "game/level/WorldDestruction.h"
#include "game/mechanics/MechanicCatalog.h"
#include "game/mechanics/MechanicsFacade.h"
#include "game/party/AttackSceneFixture.h"
#include "game/party/MoveStrikes.h"
#include "game/party/PlayerArsenal.h"
#include "game/party/PlayerAttacks.h"
#include "game/party/PlayerFamiliar.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PlayerMissiles.h"
#include "game/party/PlayerRuntime.h"
#include "game/party/PortalDeparture.h"
#include "game/party/PowerupCompanion.h"
#include "game/party/ProjectileFixture.h"
#include "game/players/PlayerAnimator.h"
#include "game/rollback/LocalOwnerContract.h"
#include "game/scenes/PlayScene.h"
#include "game/tower/HintMenu.h"
#include "game/tower/LevelArrivalPresentation.h"
#include "game/tower/SumnerFigure.h"
#include "game/tower/SumnerHints.h"
#include "game/tower/SumnerVisit.h"
#include "game/tower/TowerPromotion.h"
#include "game/tower/TowerRelics.h"
#include "game/widgets/BurnDialogueScroll.h"
#include "game/widgets/ScrollBox.h"

namespace gdl::test {
// A copy of the familiar or the companion shares its source's identity, so a copy is asked as
// well. The game neither copies nor moves either: PlayerFigure restores both in place.
template <> inline constexpr bool kCopiedOwner<game::PlayerFamiliar> = true;
template <> inline constexpr bool kCopiedOwner<game::PowerupCompanion> = true;
} // namespace gdl::test

namespace {
using namespace gdl;
using namespace gdl::game;
using test::LocalOwnerRules;
using enum test::Admission;
constexpr f32 kStep = 1.0f / 60;

/** The registry's view of what `owner` borrows, after the owner registers it when it can. A
 * cleared or moved-from owner may have nothing to register; its digest then says so. */
template <class Owner>
NativeStateDigest::Resources registered(NativeStateResources& registry, const Owner& owner) {
    if constexpr (requires { owner.registerNativeResources(registry); }) {
        (void)owner.registerNativeResources(registry);
    }
    return registry.resources();
}

// ---------------------------------------------------------------------------------------------
// The world's level: an animated floor and wall, a window, a wall that explodes, a pressure
// pad pair, a rotator, a Skorne rock, a breakable wall, a coin, a crystal and a falling piece.

/** The level, written once and read by every owner bound to it. */
const std::filesystem::path& worldStageRoot() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::sampleLevel("local-owner-world");
        std::filesystem::create_directories(dir / "wdata");
        writeTextFile(dir / "wdata/TEST.json", R"({"levels":[{"name":"TEST","bossType":42}]})");
        writeTextFile(dir / "animations.json", R"({
          "particles":[{"preset":2,"enables":16929,"texture":"STONE",
            "particleLife":[0.2,0],"rate":[30,30,30,30]}],
          "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":2}],
          "trees":[
           {"name":"COIN","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
           {"name":"GEMORANGE","nodes":[{"name":"ROOT","object":"WALL","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"IDLE","frames":4,"frameRate":30}]},
           {"name":"GETGEMORANGE","nodes":[{"name":"SPARK","particle":0,"parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"BURST","frames":6,"frameRate":30}]}]})");
        writeTextFile(dir / "world.json", R"({
          "objects":[
            {"name":"FLOOR","position":[0,0,0],"child":1,"next":2,"flags":4100},
            {"name":"WALL","position":[0,3,0],"next":-1},
            {"name":"WINDOW","position":[20,0,0],"next":3},
            {"name":"WALL","position":[40,0,0],"next":4,"flags":327680},
            {"name":"WALL","position":[60,0,0],"next":5,"flags":4096},
            {"name":"L1PSYSE_BRAZIER","position":[70,1,0],"next":-1,"flags":2048}],
          "bounds":{"min":[-20,-10,-20],"max":[100,20,20]},
          "animations":[
            {"object":0,"frames":4,"state":257,"start":0,
             "track":{"flags":32,"frames":[0,3],"values":[0,4]}},
            {"object":3,"frames":4,"state":257,"start":0,
             "track":{"flags":16,"frames":[0,3],"values":[0,2]}}],
          "particles":[{"id":"E","preset":2,"enables":16929,"texture":"STONE",
            "particleLife":[0.2,0],"rate":[30,30,30,30]}],
          "itemInfos":[
            {"type":5,"subtype":24,"name":"PAD","radius":2,"height":1},
            {"type":12,"subtype":0,"name":"SPIN","radius":2,"height":1},
            {"type":10,"subtype":40,"name":"WALL","radius":2,"height":3},
            {"type":10,"subtype":42,"name":"WALL","hitPoints":25,"armor":1,"radius":2,"height":5},
            {"type":1,"subtype":1,"name":"COIN","value":500,"radius":1,"height":2},
            {"type":1,"subtype":15,"name":"GEMORANGE","value":4,"radius":1,"height":2},
            {"type":10,"subtype":51,"name":"WALL","radius":2,"height":3}],
          "itemInstances":[
            {"info":0,"flags":2,"position":[0,0,0],"params":[0,0,2,0,4,255,1,0,0,0,0,0]},
            {"info":0,"flags":2,"position":[20,0,0],"params":[2,0,16,0,4,255,2,0,0,0,0,0]},
            {"info":1,"flags":2,"position":[60,0,0],"params":[4,0,0,0,10,215,35,60,0,0,128,63]},
            {"info":2,"name":"WALL","position":[30,0,0]},
            {"info":3,"name":"WALL","position":[50,0,5],
             "collision":[{"normal":[0,0,1],"vertices":[[-3,0,0],[3,0,0],[0,5,0]]}]},
            {"info":4,"position":[0,0,0]},
            {"info":5,"position":[80,0,0]},
            {"info":6,"name":"WALL","position":[90,0,0]}]})");
        writeTextFile(dir / "collision.json", R"({"objects":[
          {"object":0,"normals":[0,1,0],"vertices":[-3,0,-3,3,0,-3,0,0,3]}]})");
        return dir;
    }();
    return kRoot;
}

/** The world level as LevelWorld loads it. */
LevelRef worldStageRef() {
    return {"TEST", 0, "TEST", "", "", "", "", 0};
}

/** The world level's layout, meshes, textures, items, collision, scene and animator, loaded on
 * their own for the owners that borrow them, and a registry naming them in a digest. */
struct WorldStage {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    ItemArchive items;
    WorldCollision collision;
    WorldScene scene;
    WorldAnimator animator;
    NativeStateResources registry;

    WorldStage() {
        const auto& root = worldStageRoot();
        REQUIRE(layout.load(root));
        REQUIRE(models.load(root));
        REQUIRE(textures.load(root));
        REQUIRE(items.load(root));
        REQUIRE(collision.load(root, layout));
        buildScene();
        animator.bind(layout);
        REQUIRE(registry.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
        REQUIRE(registry.registerLayout(layout, "stage"));
        REQUIRE(registry.registerCollision(collision, "stage"));
        REQUIRE(registry.registerTextures(textures, "stage/textures"));
        REQUIRE(registry.registerArchive(items, "stage/items"));
    }
    void buildScene() { REQUIRE(scene.build(layout, models, textures, device)); }
    /** Names what `owner` borrows, the textures its binding made since included. */
    template <class Owner> NativeStateDigest::Resources resources(const Owner& owner) {
        (void)registry.refreshTextures(textures);
        (void)registry.refreshArchive(items);
        return registered(registry, owner);
    }
};

/** The world level's scene, its animated floor and wall moving on and its light dimming. */
struct WorldSceneRow {
    using Owner = WorldScene;
    static constexpr std::string_view kName = "WorldScene";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    WorldStage stage;
    s32 ticks = 0;

    Owner& owner() { return stage.scene; }
    void advance() {
        ++ticks;
        stage.scene.capturePresentation();
        stage.animator.step(kStep, stage.scene);
        stage.scene.setDarken(static_cast<f32>(ticks) / 10);
    }
    bool clear() {
        stage.scene.clear();
        return true;
    }
    bool rebind() {
        stage.scene.clear();
        stage.buildScene();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The world level's animator, playing its floor and wall. */
struct WorldAnimatorRow {
    using Owner = WorldAnimator;
    static constexpr std::string_view kName = "WorldAnimator";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    WorldStage stage;

    Owner& owner() { return stage.animator; }
    void advance() {
        stage.scene.capturePresentation();
        stage.animator.step(kStep, stage.scene);
    }
    bool clear() {
        stage.animator.clear();
        return true;
    }
    bool rebind() {
        stage.animator.clear();
        stage.animator.bind(stage.layout);
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The world level's collision, its floor riding up as a moving object. */
struct WorldCollisionRow {
    using Owner = WorldCollision;
    static constexpr std::string_view kName = "WorldCollision";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    WorldStage stage;
    s32 ticks = 0;

    WorldCollisionRow() { move(); }
    void move() { stage.collision.setMovingObjects(std::array<s32, 1>{0}); }
    Owner& owner() { return stage.collision; }
    void advance() {
        ++ticks;
        stage.collision.setObjectTransform(
            0, glm::translate(Mat4{1}, Vec3{0, static_cast<f32>(ticks) / 10, 0}));
    }
    bool clear() {
        stage.collision.clear();
        return true;
    }
    bool rebind() {
        stage.collision.clear();
        REQUIRE(stage.collision.load(worldStageRoot(), stage.layout));
        move();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The world level's exploding wall, blown up and then its explosion taken. */
struct WorldDestructionRow {
    using Owner = WorldDestruction;
    static constexpr std::string_view kName = "WorldDestruction";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    WorldStage stage;
    WorldDestruction destruction;

    WorldDestructionRow() { destruction.bind(stage.layout); }
    Owner& owner() { return destruction; }
    void advance() {
        if (!destruction.explode(3, {40, 0, 0}, stage.scene, stage.collision)) {
            (void)destruction.takeExplosions();
        }
    }
    bool clear() {
        destruction.clear();
        return true;
    }
    bool rebind() {
        destruction.clear();
        destruction.bind(stage.layout);
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The world level's rotator, stood on as it turns. */
struct RotatorsRow {
    using Owner = Rotators;
    static constexpr std::string_view kName = "Rotators";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    WorldStage stage;
    Rotators rotators;

    RotatorsRow() { rotators.bind(stage.layout); }
    Owner& owner() { return rotators; }
    void advance() {
        const std::array visitors{TriggerVisitor{.position = Vec3{60, 0, 0}}};
        (void)rotators.update(kStep, visitors, stage.scene);
    }
    bool clear() {
        rotators.clear();
        return true;
    }
    bool rebind() {
        rotators.clear();
        rotators.bind(stage.layout);
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The world level's Skorne rock, cued from a boss that walks. */
struct SkorneArenaRow {
    using Owner = SkorneArena;
    static constexpr std::string_view kName = "SkorneArena";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    WorldStage stage;
    SkorneArena arena;
    s32 ticks = 0;

    SkorneArenaRow() { bind(); }
    void bind() { arena.bind(stage.device, stage.layout, stage.models, stage.textures); }
    Owner& owner() { return arena; }
    void advance() {
        ++ticks;
        arena.cue({static_cast<f32>(ticks), 0, 0});
        arena.update(kStep);
    }
    bool clear() {
        arena.clear();
        return true;
    }
    bool rebind() {
        arena.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The world level's falling piece, brushed and then falling. */
struct FallingSceneryRow {
    using Owner = FallingScenery;
    static constexpr std::string_view kName = "FallingScenery";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    WorldStage stage;
    FallingScenery falling;

    FallingSceneryRow() { bind(); }
    void bind() {
        falling.bind(stage.device, stage.layout, stage.models, stage.textures, "TEST");
        falling.setPlayerCount(1);
    }
    Owner& owner() { return falling; }
    void advance() {
        (void)falling.touch({30, 0, 0}, 1);
        (void)falling.touch({90, 0, 0}, 1);
        falling.update(kStep);
    }
    bool clear() {
        falling.clear();
        return true;
    }
    bool rebind() {
        falling.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The world level's breakable wall, struck a blow at a time. */
struct DestructibleWallsRow {
    using Owner = DestructibleWalls;
    static constexpr std::string_view kName = "DestructibleWalls";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    WorldStage stage;
    DestructibleWalls walls;

    DestructibleWallsRow() { bind(); }
    void bind() {
        walls.bind(stage.device, stage.layout, stage.models, stage.textures, stage.collision);
        walls.setPlayerCount(1, stage.collision);
    }
    Owner& owner() { return walls; }
    void advance() {
        (void)walls.strike(0, 10, stage.collision);
        walls.update(kStep);
    }
    bool clear() {
        walls.clear();
        return true;
    }
    bool rebind() {
        walls.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The world level's coin and crystal, playing their loops. */
struct PlacedItemsRow {
    using Owner = PlacedItems;
    static constexpr std::string_view kName = "PlacedItems";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    WorldStage stage;
    PlacedItems placed;

    PlacedItemsRow() { bind(); }
    void bind() {
        REQUIRE(placed.bind(stage.device, stage.layout, &stage.collision,
                            std::array<ItemArchive*, 1>{&stage.items}));
        placed.setPlayerCount(1);
    }
    Owner& owner() { return placed; }
    void advance() {
        placed.capturePresentation();
        placed.update(kStep);
    }
    bool clear() {
        placed.clear();
        return true;
    }
    bool rebind() {
        placed.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The world level's pressure pads, the first stood on. */
struct LevelTriggersRow {
    using Owner = LevelTriggers;
    static constexpr std::string_view kName = "LevelTriggers";
    static constexpr LocalOwnerRules kRules{.foreign = RefusedKeepingDigest,
                                            .blank = RefusedKeepingDigest,
                                            .cleared = RefusedKeepingDigest,
                                            .restoredThenCleared = RefusedKeepingDigest,
                                            .rebound = RefusedKeepingDigest,
                                            .movedInto = RefusedKeepingDigest,
                                            .movedFrom = Refused,
                                            .assignedInto = RefusedKeepingDigest,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    WorldStage stage;
    LevelTriggers triggers;

    LevelTriggersRow() { bind(); }
    void bind() {
        triggers.bind(stage.layout, stage.animator, &stage.collision);
        triggers.setPlayerCount(1);
    }
    Owner& owner() { return triggers; }
    void advance() {
        const std::array visitors{TriggerVisitor{.position = Vec3{0, 0, 0}, .floorObject = 0}};
        triggers.update(kStep, visitors, stage.animator, stage.scene, &stage.collision);
    }
    bool clear() {
        triggers.clear();
        return true;
    }
    bool rebind() {
        triggers.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** Copies of the world level's wall stood about it, one more each step. */
struct SpawnedObjectsRow {
    using Owner = SpawnedObjects;
    static constexpr std::string_view kName = "SpawnedObjects";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Unasked,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    SpawnedObjects spawned;
    s32 ticks = 0;

    Owner& owner() { return spawned; }
    void advance() {
        ++ticks;
        REQUIRE(spawned.spawn(device, worldStageRoot(), worldStageRef(), "WALL",
                              glm::translate(Mat4{1}, Vec3{static_cast<f32>(ticks), 0, 0})));
    }
    bool clear() {
        spawned.clear();
        return true;
    }
    static bool rebind() { return false; }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** The world level loaded whole, its animations playing. */
struct LevelWorldRow {
    using Owner = LevelWorld;
    static constexpr std::string_view kName = "LevelWorld";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    LevelWorld world;
    /** Made anew with each load, since a reloaded level's assets are new to it. */
    std::unique_ptr<NativeStateResources> registry;

    LevelWorldRow() { load(); }
    void load() {
        REQUIRE(world.load(device, worldStageRoot(), worldStageRef()));
        world.setPlayerCount(1);
        registry = std::make_unique<NativeStateResources>();
        test::registerStage(*registry, world, device);
    }
    Owner& owner() { return world; }
    void advance() {
        world.capturePresentation();
        world.update(kStep);
    }
    bool clear() {
        world.clear();
        return true;
    }
    bool rebind() {
        world.clear();
        load();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(*registry, owner);
    }
};

// Every row's owner is one a roster can hold.
static_assert(LocalOwner<WorldSceneRow::Owner>);
static_assert(LocalOwner<WorldAnimatorRow::Owner>);
static_assert(LocalOwner<WorldCollisionRow::Owner>);
static_assert(LocalOwner<WorldDestructionRow::Owner>);
static_assert(LocalOwner<RotatorsRow::Owner>);
static_assert(LocalOwner<SkorneArenaRow::Owner>);
static_assert(LocalOwner<FallingSceneryRow::Owner>);
static_assert(LocalOwner<DestructibleWallsRow::Owner>);
static_assert(LocalOwner<PlacedItemsRow::Owner>);
static_assert(LocalOwner<LevelTriggersRow::Owner>);
static_assert(LocalOwner<SpawnedObjectsRow::Owner>);
static_assert(LocalOwner<LevelWorldRow::Owner>);

TEST_CASE("the world's snapshot owners keep their admission rules", "[local-owner-contract]") {
    test::checkLocalOwner<WorldSceneRow>();
    test::checkLocalOwner<WorldAnimatorRow>();
    test::checkLocalOwner<WorldCollisionRow>();
    test::checkLocalOwner<WorldDestructionRow>();
    test::checkLocalOwner<RotatorsRow>();
    test::checkLocalOwner<SkorneArenaRow>();
    test::checkLocalOwner<FallingSceneryRow>();
    test::checkLocalOwner<DestructibleWallsRow>();
    test::checkLocalOwner<PlacedItemsRow>();
    test::checkLocalOwner<LevelTriggersRow>();
    test::checkLocalOwner<SpawnedObjectsRow>();
    test::checkLocalOwner<LevelWorldRow>();
}

// ---------------------------------------------------------------------------------------------
// The fixtures' level: a locked chest, two barrels, a gate, a trap, a prop and a safe rock on
// a moving floor, with the trees they wear.

/** The level, written once and read by every fixture bound to it. */
const std::filesystem::path& fixtureStageRoot() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-fixtures");
        writeTextFile(dir / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeTextFile(dir / "objects.json", R"({"objects":[
          {"name":"TRI","file":"tri.obj"},
          {"name":"ROCK0","file":"tri.obj"},{"name":"ROCK1","file":"tri.obj"},
          {"name":"ROCK2","file":"tri.obj"},{"name":"ROCK3","file":"tri.obj"}]})");
        writeFile(dir / "white.png", test::kTinyPng);
        writeTextFile(dir / "textures.json", R"({"bitmaps":[
          {"name":"WHITE","file":"white.png","width":2,"height":2},
          {"name":"WHITE1","file":"white.png","width":2,"height":2}]})");
        writeTextFile(dir / "world.json", R"({
          "objects":[{"name":"TRI","position":[0,0,0],"flags":4100}],
          "itemInfos":[
            {"type":2,"subtype":46,"name":"CHEST","radius":1,"height":2,"activeType":16},
            {"type":10,"subtype":43,"name":"BARREL","radius":1,"height":3,"hitPoints":10,"armor":1},
            {"type":10,"subtype":45,"name":"BARREL","radius":1,"height":3,"hitPoints":5},
            {"type":7,"name":"GATE","radius":1,"height":3},
            {"type":8,"name":"TRAP","radius":2,"height":3,"collisionType":3,"value":5,"activeOff":-4},
            {"type":10,"subtype":0,"name":"PROP","radius":1,"height":2},
            {"type":10,"subtype":41,"name":"ROCK","radius":2,"height":5,"hitPoints":25},
            {"type":1,"subtype":2,"name":"KEY","radius":1,"value":1},
            {"type":1,"subtype":1,"name":"KEY","radius":1,"value":200},
            {"type":-1,"choices":[7,8]}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[9,0,0,0,1,0,0,0,0,0,0,0]},
            {"info":1,"position":[20,0,0]},
            {"info":2,"position":[40,0,0]},
            {"info":3,"position":[60,0,0]},
            {"info":4,"position":[80,0,0]},
            {"info":5,"position":[100,0,0]},
            {"info":6,"position":[120,0,0],"params":[41,0,3,0,0,0,0,0,0,0,0,0]}]})");
        std::string trees;
        for (const auto* const name :
             {"CHEST", "BARREL", "GATE", "TRAP", "TRAP_D", "PROP", "KEY"}) {
            if (!trees.empty()) {
                trees += ',';
            }
            trees += std::string{R"({"name":")"} + name + R"(","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
              {"name":"NULL1","parent":0,"position":[0,1,0]}],
              "sequences":[{"name":"REST","frames":4,"frameRate":30},
                {"name":"ACTIVE","frames":5,"frameRate":30},
                {"name":"HIT","frames":6,"frameRate":30},
                {"name":"BACK","frames":5,"frameRate":30}]})";
        }
        writeTextFile(dir / "animations.json",
                      R"({"textureAnimations":[{"name":"WHITE","texture":0,"source":0,
                       "frames":2,"rate":2}],"trees":[)" +
                          trees + "]}");
        test::convertModelFixture(dir);
        return dir;
    }();
    return kRoot;
}

/** The fixtures' level loaded, its floor a moving object, and a registry naming it. */
struct FixtureStage {
    test::FakeRenderDevice device;
    WorldLayout layout;
    WorldCollision collision;
    ItemArchive items;
    NativeStateResources registry;

    FixtureStage() {
        REQUIRE(layout.load(fixtureStageRoot()));
        REQUIRE(items.load(fixtureStageRoot()));
        collision.build(
            {{{0, 1, 0}, {Vec3{-20, 0, -20}, Vec3{200, 0, -20}, Vec3{-20, 0, 20}}, 0, 4100},
             {{0, 1, 0}, {Vec3{200, 0, -20}, Vec3{200, 0, 20}, Vec3{-20, 0, 20}}, 0, 4100}});
        collision.setMovingObjects(std::array<s32, 1>{0});
        collision.setObjectTransform(0, Mat4{1});
        REQUIRE(registry.registerArchive(items, "fixtures/items"));
        REQUIRE(registry.registerLayout(layout, "fixtures/stage"));
        REQUIRE(registry.registerCollision(collision, "fixtures/stage"));
        REQUIRE(registry.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
    }
    AssetScope scope() { return AssetScope{items}; }
    /** Names what `owner` borrows, the textures its binding made since included. */
    template <class Owner> NativeStateDigest::Resources resources(const Owner& owner) {
        (void)registry.refreshArchive(items);
        return registered(registry, owner);
    }
};

/** The fixtures' locked chest, opened by a visitor with a key. */
struct ChestsRow {
    using Owner = Chests;
    static constexpr std::string_view kName = "Chests";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    FixtureStage stage;
    Chests chests;

    ChestsRow() { bind(); }
    void bind() {
        REQUIRE(chests.bind(stage.device, stage.layout, stage.scope(), &stage.collision));
        chests.setPlayerCount(1);
    }
    Owner& owner() { return chests; }
    void advance() {
        chests.capturePresentation();
        const std::array party{ChestVisitor{{0, 0, 0}, 1, 1}};
        (void)chests.update(kStep, party);
    }
    bool clear() {
        chests.clear();
        return true;
    }
    bool rebind() {
        chests.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The fixtures' barrels, the first struck a blow at a time. */
struct BreakablesRow {
    using Owner = Breakables;
    static constexpr std::string_view kName = "Breakables";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    FixtureStage stage;
    Breakables barrels;

    BreakablesRow() { bind(); }
    void bind() {
        REQUIRE(barrels.bind(stage.device, stage.layout, stage.scope(), &stage.collision));
    }
    Owner& owner() { return barrels; }
    void advance() {
        barrels.capturePresentation();
        (void)barrels.strike(0, 2);
        (void)barrels.update(kStep);
    }
    bool clear() {
        barrels.clear();
        return true;
    }
    bool rebind() {
        barrels.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The fixtures' gate, opened by a visitor with a key. */
struct LockedGatesRow {
    using Owner = LockedGates;
    static constexpr std::string_view kName = "LockedGates";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    FixtureStage stage;
    LockedGates gates;

    LockedGatesRow() { bind(); }
    void bind() {
        REQUIRE(gates.bind(stage.device, stage.layout, stage.scope(), &stage.collision));
    }
    Owner& owner() { return gates; }
    void advance() {
        gates.capturePresentation();
        const std::array party{ChestVisitor{{60, 0, 0}, 1, 1}};
        (void)gates.update(1, kStep, party);
    }
    bool clear() {
        gates.clear();
        return true;
    }
    bool rebind() {
        gates.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The fixtures' trap, cycling with a victim in its box. */
struct TrapsRow {
    using Owner = Traps;
    static constexpr std::string_view kName = "Traps";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    FixtureStage stage;
    Traps traps;

    TrapsRow() { bind(); }
    void bind() {
        REQUIRE(traps.bind(stage.device, stage.layout, stage.scope(), &stage.collision, 17));
    }
    Owner& owner() { return traps; }
    void advance() {
        traps.capturePresentation();
        const std::array victims{TrapVictim{{80, 1.5f, 0}, 1, 1.5f}};
        (void)traps.update(1, kStep, victims, false);
    }
    bool clear() {
        traps.clear();
        return true;
    }
    bool rebind() {
        traps.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The fixtures' prop, playing its tree. */
struct StaticSceneryRow {
    using Owner = StaticScenery;
    static constexpr std::string_view kName = "StaticScenery";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    FixtureStage stage;
    StaticScenery scenery;

    StaticSceneryRow() { bind(); }
    void bind() { scenery.bind(stage.device, stage.layout, stage.scope(), &stage.collision); }
    Owner& owner() { return scenery; }
    void advance() {
        scenery.capturePresentation();
        scenery.update(kStep);
    }
    bool clear() {
        scenery.clear();
        return true;
    }
    bool rebind() {
        scenery.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The fixtures' safe rock, struck a blow at a time. */
struct SafeRocksRow {
    using Owner = SafeRocks;
    static constexpr std::string_view kName = "SafeRocks";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    FixtureStage stage;
    SafeRocks rocks;

    SafeRocksRow() { bind(); }
    void bind() {
        REQUIRE(rocks.bind(stage.device, stage.layout, stage.items));
        rocks.bindAnimations(stage.device, stage.items);
    }
    Owner& owner() { return rocks; }
    void advance() {
        rocks.capturePresentation();
        rocks.strike(0, 2);
        rocks.update(kStep);
    }
    bool clear() {
        rocks.clear();
        return true;
    }
    bool rebind() {
        rocks.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** Rubble left about the fixtures' level, a piece more each step. */
struct RubbleRow {
    using Owner = Rubble;
    static constexpr std::string_view kName = "Rubble";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Unasked,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    FixtureStage stage;
    Rubble rubble;
    s32 ticks = 0;

    Owner& owner() { return rubble; }
    void advance() {
        ++ticks;
        const std::array archives{&stage.items};
        REQUIRE(rubble.leave(stage.device, archives, "TRI",
                             glm::translate(Mat4{1}, Vec3{static_cast<f32>(ticks), 0, 0})));
    }
    bool clear() {
        rubble.clear();
        return true;
    }
    static bool rebind() { return false; }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** A level's fixtures over an empty world, a blast going off a little further each step
 * between two players. */
struct LevelFixturesRow {
    using Owner = LevelFixtures;
    static constexpr std::string_view kName = "LevelFixtures";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    std::array<PlayerRuntime, 2> players;
    test::ScriptedFixtures host;
    NativeStateResources registry;
    s32 ticks = 0;

    LevelFixturesRow() {
        bind();
        players[0].actor.spawn(3, {}, nullptr, {0, 0, 0}, 0);
        players[1].actor.spawn(1, {}, nullptr, {6, 0, 0}, 0);
        test::registerStage(registry, world, device);
    }
    ~LevelFixturesRow() { fixtures.clear(); }
    LevelFixturesRow(const LevelFixturesRow&) = delete;
    LevelFixturesRow& operator=(const LevelFixturesRow&) = delete;
    LevelFixturesRow(LevelFixturesRow&&) = delete;
    LevelFixturesRow& operator=(LevelFixturesRow&&) = delete;
    void bind() { fixtures.bind({device, world, weapons, effects, audio, 1}); }
    Owner& owner() { return fixtures; }
    void advance() {
        ++ticks;
        fixtures.blast({static_cast<f32>(ticks), 0, 0}, LevelFixtures::kBlastRadius, 20, players,
                       host);
        fixtures.advanceBlasts(kStep, players, host);
    }
    bool clear() {
        fixtures.clear();
        return true;
    }
    bool rebind() {
        fixtures.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

static_assert(LocalOwner<LevelFixturesRow::Owner>);
static_assert(LocalOwner<ChestsRow::Owner>);
static_assert(LocalOwner<BreakablesRow::Owner>);
static_assert(LocalOwner<LockedGatesRow::Owner>);
static_assert(LocalOwner<TrapsRow::Owner>);
static_assert(LocalOwner<StaticSceneryRow::Owner>);
static_assert(LocalOwner<SafeRocksRow::Owner>);
static_assert(LocalOwner<RubbleRow::Owner>);

TEST_CASE("the fixtures' snapshot owners keep their admission rules", "[local-owner-contract]") {
    test::checkLocalOwner<LevelFixturesRow>();
    test::checkLocalOwner<ChestsRow>();
    test::checkLocalOwner<BreakablesRow>();
    test::checkLocalOwner<LockedGatesRow>();
    test::checkLocalOwner<TrapsRow>();
    test::checkLocalOwner<StaticSceneryRow>();
    test::checkLocalOwner<SafeRocksRow>();
    test::checkLocalOwner<RubbleRow>();
}

// ---------------------------------------------------------------------------------------------
// The travel level: an exit portal, a secret one and two transporter pads on a floor, with the
// trees an arrival, a departure and the boss's wizard wear.

/** The level, written once and read by every travel owner bound to it. */
const std::filesystem::path& travelStageRoot() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::sampleLevel("local-owner-travel");
        std::filesystem::create_directories(dir / "wdata");
        writeTextFile(dir / "wdata/TOWN.json", R"({"realm":7,"prefix":"levelG",
          "levels":[{"name":"G1","title":"Town"}]})");
        writeTextFile(dir / "wdata/SECRET.json", R"({"realm":12,"prefix":"levelS",
          "levels":[{"name":"S1","title":"Secret"}]})");
        writeTextFile(dir / "world.json", R"({
          "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4102}],
          "itemInfos":[{"type":9,"name":"EXIT_PORTAL","radius":3},
            {"type":9,"subtype":50,"name":"SECRET_ICON","radius":1},
            {"type":11,"name":"TRANS","radius":2,"height":2}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[0,0,0,0,103,49,0,0]},
            {"info":1,"position":[20,0,0],"params":[0,0,0,0,83,49,0,0]},
            {"info":2,"position":[-10,0,0],"params":[1,0,0,0,2,0,0,0]},
            {"info":2,"position":[10,0,0],"params":[2,0,0,0,1,0,0,0]}]})");
        writeTextFile(dir / "animations.json", R"({
          "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":1}],
          "trees":[
            {"name":"EXIT_PORTAL","nodes":[{"name":"BODY","object":"WALL","position":[0,0,0]}],
             "sequences":[{"name":"IDLE","frames":4,"frameRate":30},
              {"name":"READY","frames":4,"frameRate":30},
              {"name":"ACTIVE1","frames":4,"frameRate":30},
              {"name":"ACTIVE2","frames":4,"frameRate":30},
              {"name":"ACTIVE3","frames":4,"frameRate":30}]},
            {"name":"SECRET_ICON","nodes":[{"name":"BODY","object":"WALL","position":[0,0,0]}],
             "sequences":[{"name":"IDLE","frames":9,"frameRate":30}]},
            {"name":"TRANS","nodes":[{"name":"BODY","object":"WALL","position":[0,0,0]}],
             "sequences":[{"name":"ACTIVE","frames":8,"frameRate":30}]},
            {"name":"STARTFX","nodes":[{"name":"BODY","object":"WALL","position":[0,0,0]}],
             "sequences":[{"name":"START","frames":8,"frameRate":30}]},
            {"name":"WIZARD","nodes":[{"name":"BODY","object":"WALL","position":[0,0,0]}],
             "sequences":[{"name":"READY","frames":8,"frameRate":30}]}]})");
        return dir;
    }();
    return kRoot;
}

/** The travel level loaded on its floor, and a registry naming it. */
struct TravelStage {
    test::FakeRenderDevice device;
    ItemArchive items;
    WorldLayout layout;
    WorldCollision floor;
    LevelCatalog catalog;
    NativeStateResources registry;

    TravelStage() {
        REQUIRE(items.load(travelStageRoot()));
        REQUIRE(layout.load(travelStageRoot()));
        REQUIRE(catalog.load(travelStageRoot()));
        floor.build(
            {{.vertices = {Vec3{-40, 0, -40}, Vec3{40, 0, 40}, Vec3{40, 0, -40}}, .object = 0},
             {.vertices = {Vec3{-40, 0, -40}, Vec3{-40, 0, 40}, Vec3{40, 0, 40}}, .object = 0}});
        floor.setMovingObjects(std::array<s32, 1>{0});
        REQUIRE(registry.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
        REQUIRE(registry.registerArchive(items, "items/travel"));
        REQUIRE(registry.registerLayout(layout, "travel"));
        REQUIRE(registry.registerCollision(floor, "travel"));
    }
    AssetScope scope() { return AssetScope{items}; }
    /** Names what `owner` borrows, the textures its binding made since included. */
    template <class Owner> NativeStateDigest::Resources resources(const Owner& owner) {
        (void)registry.refreshArchive(items);
        return registered(registry, owner);
    }
};

/** The travel level's portals, the party standing on the first. */
struct ExitPortalsRow {
    using Owner = ExitPortals;
    static constexpr std::string_view kName = "ExitPortals";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    TravelStage stage;
    ExitPortals exits;

    ExitPortalsRow() { bind(); }
    void bind() {
        REQUIRE(exits.bind(stage.device, stage.layout, stage.scope(), stage.catalog, &stage.floor));
    }
    Owner& owner() { return exits; }
    void advance() {
        const std::array party{PortalVisitor{{0, 0, 0}, 0.75f, 0, true}};
        (void)exits.update(1, kStep, party);
    }
    bool clear() {
        exits.clear();
        return true;
    }
    bool rebind() {
        exits.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The travel level's transporter pads, playing their trees. */
struct LevelTransportersRow {
    using Owner = LevelTransporters;
    static constexpr std::string_view kName = "LevelTransporters";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    TravelStage stage;
    LevelTransporters pads;

    LevelTransportersRow() { bind(); }
    void bind() { pads.bind(stage.device, stage.layout, stage.scope(), 2, &stage.floor); }
    Owner& owner() { return pads; }
    void advance() {
        pads.capturePresentation();
        pads.animate(kStep);
    }
    bool clear() {
        pads.clear();
        return true;
    }
    bool rebind() {
        pads.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** A two-player arrival on the travel level, its effects and camera under way. */
struct LevelArrivalPresentationRow {
    using Owner = LevelArrivalPresentation;
    static constexpr std::string_view kName = "LevelArrivalPresentation";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    TravelStage stage;
    LevelArrivalPresentation arrival;

    LevelArrivalPresentationRow() { begin(); }
    void begin() {
        const std::array party{Vec3{-2, 0, 10}, Vec3{2, 0, 10}};
        arrival.begin(stage.device, stage.items, party, WorldCamera{});
    }
    Owner& owner() { return arrival; }
    void advance() {
        arrival.capturePresentation();
        arrival.animate(kStep);
        arrival.advance(1, false, {0, 10, -10}, {0, 0, 10});
    }
    bool clear() {
        arrival.clear();
        return true;
    }
    bool rebind() {
        arrival.clear();
        begin();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** A departure through the travel level's skin, sinking a tick at a time. */
struct PortalDepartureRow {
    using Owner = PortalDeparture;
    static constexpr std::string_view kName = "PortalDeparture";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    TravelStage stage;
    PortalDeparture departure;

    PortalDepartureRow() { departure.loadSkin(stage.device, stage.items.textures); }
    Owner& owner() { return departure; }
    void advance() {
        if (!departure.started()) {
            departure.begin(stage.device, stage.items.textures);
        }
        departure.update(1);
    }
    bool clear() {
        departure.clear();
        return true;
    }
    bool rebind() {
        departure.clear();
        departure.loadSkin(stage.device, stage.items.textures);
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The boss's wizard on the travel level, his visit begun beside a party of one. */
struct BossVictoryPresentationRow {
    using Owner = BossVictoryPresentation;
    static constexpr std::string_view kName = "BossVictoryPresentation";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    TravelStage stage;
    const MessageTable strings;
    BossVictoryPresentation victory;

    BossVictoryPresentationRow() { begin(); }
    void begin() {
        victory.begin(41, 'G', 1, 0, false);
        victory.bindWizard(stage.device, stage.items, Vec3{0}, std::array{Vec3{2, 0, 2}});
    }
    Owner& owner() { return victory; }
    void advance() {
        victory.capturePresentation();
        (void)victory.update(1, kStep, false, strings);
    }
    bool clear() {
        victory.clear();
        return true;
    }
    bool rebind() {
        victory.clear();
        begin();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

static_assert(LocalOwner<ExitPortalsRow::Owner>);
static_assert(LocalOwner<LevelTransportersRow::Owner>);
static_assert(LocalOwner<LevelArrivalPresentationRow::Owner>);
static_assert(LocalOwner<PortalDepartureRow::Owner>);
static_assert(LocalOwner<BossVictoryPresentationRow::Owner>);

TEST_CASE("the travel snapshot owners keep their admission rules", "[local-owner-contract]") {
    test::checkLocalOwner<ExitPortalsRow>();
    test::checkLocalOwner<LevelTransportersRow>();
    test::checkLocalOwner<LevelArrivalPresentationRow>();
    test::checkLocalOwner<PortalDepartureRow>();
    test::checkLocalOwner<BossVictoryPresentationRow>();
}

// ---------------------------------------------------------------------------------------------
// The screens' widgets and the HUD.

/** A font of every printable letter, eight wide, for the hint scrolls. */
BitmapFont printableFont() {
    std::vector<BitmapGlyph> glyphs;
    for (s32 code = 33; code < 127; ++code) {
        glyphs.push_back({code, 8, 0, 0});
    }
    return BitmapFont::fromGlyphs(10, 4, std::move(glyphs));
}

/** A scroll's font, sheet and art, and a registry naming the art. */
struct ScrollStage {
    test::FakeRenderDevice device;
    const BitmapFont font = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}, {'B', 8, 6, 0}});
    const test::FakeTexture sheet{32, 16};
    const test::FakeTexture backdrop{64, 64};
    const test::FakeTexture ring{2, 2};
    const Image source = Image::filled(4, 4, Color::rgba(200, 180, 120, 255));
    const Image mask = Image::filled(2, 2, Color::white());
    const Image burnt = Image::filled(2, 2, Color::white().withAlpha(0));
    TextPainter text;
    NativeStateResources registry;

    ScrollStage() {
        text.setFont(&font, &sheet);
        REQUIRE(registry.registerBuiltinTexture(backdrop, "test/hud/backdrop"));
        REQUIRE(registry.registerBuiltinTexture(ring, "test/hud/ring"));
        REQUIRE(registry.registerImage(source, "test/hud/source"));
        REQUIRE(registry.registerImage(mask, "test/hud/mask/0"));
        REQUIRE(registry.registerImage(burnt, "test/hud/mask/1"));
    }
    template <class Owner> NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** A two-page scroll, its pages turned and then burnt away. */
struct ScrollBoxRow {
    using Owner = ScrollBox;
    static constexpr std::string_view kName = "ScrollBox";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Admitted,
                                            .restoredThenCleared = Admitted,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    ScrollStage stage;
    ScrollBox box;

    ScrollBoxRow() {
        setText();
        setArt();
        open();
    }
    void setText() { box.setText(&stage.text); }
    void setArt() {
        box.setArt({&stage.backdrop,
                    &stage.source,
                    nullptr,
                    nullptr,
                    {&stage.mask, &stage.burnt},
                    {&stage.ring}});
    }
    /** Leaves the scroll without a presentation of its own, as clearing a level's messages
     * does. */
    void setPresentation() { box.setPresentation(nullptr); }
    void open() { REQUIRE(box.open(stage.device, {"AB", "B"}, 1, "AB")); }
    Owner& owner() { return box; }
    void advance() { box.step(1, 1); }
    bool clear() {
        box.close();
        return true;
    }
    bool rebind() {
        box.close();
        setText();
        setArt();
        open();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** A dialogue scroll burning away. */
struct BurnDialogueScrollRow {
    using Owner = BurnDialogueScroll;
    static constexpr std::string_view kName = "BurnDialogueScroll";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Admitted,
                                            .restoredThenCleared = Admitted,
                                            .rebound = Admitted,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    ScrollStage stage;
    BurnDialogueScroll burn;

    BurnDialogueScrollRow() { start(); }
    void start() {
        REQUIRE(burn.start(stage.device, {1, 2, 3, 4}, stage.source, {&stage.mask}, {&stage.ring}));
    }
    Owner& owner() { return burn; }
    void advance() { burn.step(7); }
    bool clear() {
        burn.reset();
        return true;
    }
    bool rebind() {
        burn.reset();
        start();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) { return stage.resources(owner); }
};

/** The help lines' texts, written once. */
const std::filesystem::path& helpMessagesFile() {
    static const std::filesystem::path kFile = [] {
        const auto dir = test::scratchDirectory("local-owner-help");
        writeTextFile(dir / "messages.json",
                      R"({"messages":[{"name":"LEVELUP","lines":["LEVEL %d"]}]})");
        return dir / "messages.json";
    }();
    return kFile;
}

/** The party's HUD with a level-up line up, ageing a tick at a time. */
struct PartyHudRow {
    using Owner = PartyHud;
    static constexpr std::string_view kName = "PartyHud";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    MessageTable text;
    test::FakeRenderDevice device;
    ItemArchive powerups; ///< without the hourglass's art
    PartyHud hud;
    NativeStateResources registry;

    PartyHudRow() {
        REQUIRE(text.load(helpMessagesFile()));
        show();
    }
    void show() {
        hud.help().setTexts(&text);
        REQUIRE(hud.help().post(HelpMessages::kLevelUp, 0, {}, 2));
    }
    void bindHourglass() { (void)hud.bindHourglass(device, powerups); }
    Owner& owner() { return hud; }
    void advance() { hud.help().update(1); }
    bool clear() {
        hud.clear();
        return true;
    }
    bool rebind() {
        hud.clear();
        show();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** The level messages' art, fonts and texts, written once. */
const std::filesystem::path& messageStageRoot() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-messages");
        std::filesystem::create_directories(dir / "STATIC");
        std::filesystem::create_directories(dir / "fonts");
        std::filesystem::create_directories(dir / "text");
        writeFile(dir / "STATIC/tile.png", test::kTinyPng);
        constexpr std::array kNames{"FONT32",           "SCROLL_A",         "FONT32_GLOW",
                                    "GREENCIRCTRANS",   "GREENCIRCTRANS+1", "GREENCIRCTRANSM",
                                    "GREENCIRCTRANSM+1"};
        std::string manifest = R"({"bitmaps":[)";
        for (usize i = 0; i < kNames.size(); ++i) {
            if (i != 0) {
                manifest += ',';
            }
            manifest +=
                std::format(R"({{"index":{},"name":"{}","file":"tile.png","width":2,"height":2}})",
                            i, kNames[i]);
        }
        writeTextFile(dir / "STATIC/textures.json", manifest + "]}");
        writeTextFile(dir / "fonts/font32.json", R"({"height":10,"glyphs":[
            {"code":65,"width":6,"x":0,"y":0},{"code":66,"width":8,"x":0,"y":0}]})");
        writeTextFile(dir / "text/scroll_e.json", R"({"fonts":["font32"],"messages":[
            {"name":"WELCOME","font":0,"scale":1,"lines":["A","B"]}],"lists":[]})");
        return dir;
    }();
    return kRoot;
}

/** The welcome message up on its scroll, turned a page at a time. */
struct LevelMessagesRow {
    using Owner = LevelMessages;
    static constexpr std::string_view kName = "LevelMessages";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    TextureSet textures;
    LevelMessages messages;
    /** Made anew with each load, since reloaded art is new to it. */
    std::unique_ptr<NativeStateResources> registry;

    LevelMessagesRow() { open(); }
    void open() {
        messages.load(device, textures, messageStageRoot(), nullptr);
        REQUIRE(messages.nativeReady());
        registry = std::make_unique<NativeStateResources>();
        REQUIRE(registry->registerTextures(textures, "STATIC"));
        REQUIRE(messages.open(device, "WELCOME", nullptr, 1));
    }
    Owner& owner() { return messages; }
    void advance() { (void)messages.step(1, 1); }
    bool clear() {
        messages.clear();
        return true;
    }
    bool rebind() {
        messages.clear();
        open();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(*registry, owner);
    }
};

/** Writes a one-triangle archive into `dir` wearing the textures `bitmaps` names (a list of
 * bitmap records, each drawn from one tiny image) and holding `animations`. */
void writeTriangleArchive(const std::filesystem::path& dir, std::string_view bitmaps,
                          std::string_view animations) {
    writeTextFile(dir / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTextFile(dir / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
    writeFile(dir / "white.png", test::kTinyPng);
    writeTextFile(dir / "textures.json",
                  std::string{R"({"bitmaps":[)"} + std::string{bitmaps} + "]}");
    writeTextFile(dir / "animations.json", animations);
    test::convertModelFixture(dir);
}

/** The secret challenge's hourglass art, written once. */
const std::filesystem::path& hourglassArchive() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-hourglass");
        writeTriangleArchive(dir, R"(
          {"name":"TIMER","file":"white.png","width":2,"height":2},
          {"name":"TIMER_SAND","file":"white.png","width":2,"height":2},
          {"name":"SAND0","file":"white.png","width":2,"height":2},
          {"name":"SAND1","file":"white.png","width":2,"height":2})",
                             R"({"textureAnimations":[{"name":"SAND_ANIM","texture":1,
                               "source":2,"frames":2,"rate":1}],"trees":[]})");
        return dir;
    }();
    return kRoot;
}

/** The secret challenge's hourglass, its sand falling. */
struct ChallengeHudRow {
    using Owner = ChallengeHud;
    static constexpr std::string_view kName = "ChallengeHud";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    ItemArchive archive;
    ChallengeHud hud;
    NativeStateResources registry;

    ChallengeHudRow() {
        REQUIRE(archive.load(hourglassArchive()));
        bind();
        REQUIRE(registry.registerArchive(archive, "POWERUPS"));
    }
    void bind() { REQUIRE(hud.bind(device, archive)); }
    Owner& owner() { return hud; }
    void advance() { hud.step(0.21f); }
    bool clear() {
        hud.clear();
        return true;
    }
    bool rebind() {
        hud.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** The layout of a boss meter of two pieces. */
HealthMeterDefinition meterDefinition() {
    HealthMeterDefinition meter;
    meter.pieces = 2;
    meter.advance = 256;
    meter.leftInset = 44;
    meter.rightInset = 53;
    meter.shown = true;
    meter.backed = true;
    return meter;
}

/** One boss's meter, draining as its boss is hurt. */
struct BossMeterRow {
    using Owner = BossMeter;
    static constexpr std::string_view kName = "BossMeter";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    BossMeter meter;
    s32 ticks = 0;

    BossMeterRow() { bind(); }
    void bind() { REQUIRE(meter.bind(meterDefinition(), nullptr)); }
    Owner& owner() { return meter; }
    void advance() {
        ++ticks;
        meter.update(1, 1000.0f - static_cast<f32>(ticks * 100), 1000, true, false);
    }
    bool clear() {
        meter.clear();
        return true;
    }
    bool rebind() {
        meter.clear();
        bind();
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** A three-part boss's meters, each part draining at its own pace. */
struct BossMetersRow {
    using Owner = BossMeters;
    static constexpr std::string_view kName = "BossMeters";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    std::array<HealthMeterReading, 3> readings;
    BossMeters meters;
    s32 ticks = 0;

    BossMetersRow() {
        for (usize i = 0; i < readings.size(); ++i) {
            readings[i].definition = meterDefinition();
            readings[i].definition.backed = i == 0;
            readings[i].health = readings[i].maxHealth = 1200 + static_cast<f32>(i) * 300;
        }
        meters.bind(readings, nullptr);
    }
    Owner& owner() { return meters; }
    void advance() {
        ++ticks;
        auto frame = readings;
        for (usize i = 0; i < frame.size(); ++i) {
            frame[i].health -= static_cast<f32>(ticks) * static_cast<f32>(100 * (i + 1));
        }
        meters.update(1, frame, true, false);
    }
    bool clear() {
        meters.clear();
        return true;
    }
    bool rebind() {
        meters.clear();
        meters.bind(readings, nullptr);
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

static_assert(LocalOwner<ScrollBoxRow::Owner>);
static_assert(LocalOwner<BurnDialogueScrollRow::Owner>);
static_assert(LocalOwner<PartyHudRow::Owner>);
static_assert(LocalOwner<LevelMessagesRow::Owner>);
static_assert(LocalOwner<ChallengeHudRow::Owner>);
static_assert(LocalOwner<BossMeterRow::Owner>);
static_assert(LocalOwner<BossMetersRow::Owner>);

TEST_CASE("the widgets' and HUD's snapshot owners keep their admission rules",
          "[local-owner-contract]") {
    test::checkLocalOwner<ScrollBoxRow>();
    test::checkLocalOwner<BurnDialogueScrollRow>();
    test::checkLocalOwner<PartyHudRow>();
    test::checkLocalOwner<LevelMessagesRow>();
    test::checkLocalOwner<ChallengeHudRow>();
    test::checkLocalOwner<BossMeterRow>();
    test::checkLocalOwner<BossMetersRow>();
    // The scroll given its text, its art or no presentation alone, and opened anew; the HUD
    // given its hourglass.
    CHECK(test::askAfter<ScrollBoxRow>(&ScrollBoxRow::setText) == Refused);
    CHECK(test::askAfter<ScrollBoxRow>(&ScrollBoxRow::setArt) == Refused);
    CHECK(test::askAfter<ScrollBoxRow>(&ScrollBoxRow::setPresentation) == Refused);
    CHECK(test::askAfter<ScrollBoxRow>(&ScrollBoxRow::open) == Admitted);
    CHECK(test::askAfter<PartyHudRow>(&PartyHudRow::bindHourglass) == Refused);
}

// ---------------------------------------------------------------------------------------------
// The tower: Sumner, his hints and visit, and the ceremonies.

/** Sumner's figure and his lookout, written once. */
const std::filesystem::path& sumnerArchive() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-sumner");
        writeTextFile(dir / "body.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(dir / "objects.json", R"({"objects":[
          {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
        writeFile(dir / "skin.png", test::kTinyPng);
        writeTextFile(dir / "textures.json", R"({"bitmaps":[
          {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
        std::string sequences;
        for (usize i = 0; i < SumnerFigure::kSequences.size(); ++i) {
            if (i > 0) {
                sequences += ',';
            }
            sequences += R"({"name":")" + std::string(SumnerFigure::kSequences[i]) +
                         R"(","frames":4,"frameRate":30,"repeats":)" + (i < 3 ? "true" : "false") +
                         R"(,"tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[)" +
                         std::to_string(i * 10) + ',' + std::to_string(i * 10 + 3) + "]}]}";
        }
        writeTextFile(dir / "animations.json", R"({"trees":[{"name":"GWIZ","nodes":[
          {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],"sequences":[)" +
                                                   sequences + "]}]}");
        test::convertModelFixture(dir);
        writeTextFile(dir / "world.json", R"({"objects":[{"name":"GROUND","position":[0,0,0]}],
          "locators":[{"type":"event","delay":0,"position":[3,2,1],"rotation":[0,0.5,0]}]})");
        return dir;
    }();
    return kRoot;
}

/** Sumner at his lookout, sweeping towards the crystals and then idling. */
struct SumnerFigureRow {
    using Owner = SumnerFigure;
    static constexpr std::string_view kName = "SumnerFigure";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    SumnerFigure figure;
    NativeStateResources registry;
    s32 ticks = 0;

    SumnerFigureRow() {
        REQUIRE(layout.load(sumnerArchive()));
        REQUIRE(items.load(sumnerArchive()));
        load();
        REQUIRE(registry.registerArchive(items, "sumner"));
    }
    void load() { REQUIRE(figure.load(device, items, layout)); }
    Owner& owner() { return figure; }
    void advance() {
        ++ticks;
        if (ticks == 1) {
            figure.gesture();
        }
        figure.capturePresentation();
        figure.update(kStep);
    }
    bool clear() {
        figure.clear();
        return true;
    }
    bool rebind() {
        figure.clear();
        load();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** A guardian hint, written once. */
const std::filesystem::path& guardianHintsFile() {
    static const std::filesystem::path kFile = [] {
        const auto dir = test::scratchDirectory("local-owner-hints");
        writeTextFile(dir / "hints.json",
                      R"({"messages":[{"name":"G1","lines":["Guardian"]}],
                          "lists":[{"name":"BOSS_HINTS","messages":[0]}]})");
        return dir / "hints.json";
    }();
    return kFile;
}

/** Sumner's hints, asked about the open world's guardian until the search widens. */
struct SumnerHintsRow {
    using Owner = SumnerHints;
    static constexpr std::string_view kName = "SumnerHints";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Unasked,
                                            .restoredThenCleared = Unasked,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    SumnerHints hints;

    SumnerHintsRow() { load(); }
    void load() { REQUIRE(hints.load(guardianHintsFile())); }
    Owner& owner() { return hints; }
    void advance() {
        HintKnowledge knowledge;
        knowledge.worldsOpen = 1U << 7U;
        (void)hints.next(HintTopic::Guardians, knowledge, "GENERAL");
    }
    static bool clear() { return false; }
    bool rebind() {
        load();
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** The general hints Sumner's visit scroll reads, written once. */
const std::filesystem::path& generalHintsFile() {
    static const std::filesystem::path kFile = [] {
        const auto dir = test::scratchDirectory("local-owner-visit");
        writeTextFile(dir / "hints.json", R"({
            "fonts": ["font32"],
            "messages": [{"name": "GENERAL", "font": 0, "scale": 1,
                          "lines": ["FIRST", "SECOND"]}],
            "lists": [{"name": "GENERAL_HINTS", "messages": [0]}]})");
        return dir / "hints.json";
    }();
    return kFile;
}

/** Sumner's visit, the first player stepping up to be greeted. */
struct SumnerVisitRow {
    using Owner = SumnerVisit;
    static constexpr std::string_view kName = "SumnerVisit";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    BitmapFont glyphs = printableFont();
    test::FakeTexture sheet{64, 64};
    TextPainter text;
    GameConfig config;
    StringTable strings;
    SumnerVisit visit;

    SumnerVisitRow() {
        text.setFont(&glyphs, &sheet);
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        load();
    }
    void load() { REQUIRE(visit.loadTexts(generalHintsFile())); }
    Owner& owner() { return visit; }
    void advance() { (void)visit.visit(0.25f, 0, true, text, &config, &strings); }
    bool clear() {
        visit.clear();
        return true;
    }
    bool rebind() {
        visit.clear();
        load();
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** Sumner's hint scroll with two topics, unrolling. */
struct HintMenuRow {
    using Owner = HintMenu;
    static constexpr std::string_view kName = "HintMenu";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Admitted,
                                            .restoredThenCleared = Admitted,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    BitmapFont glyphs = printableFont();
    test::FakeTexture sheet{64, 64};
    TextPainter painter;
    HintMenu menu;

    HintMenuRow() {
        painter.setFont(&glyphs, &sheet);
        setArt();
        open();
    }
    /** Gives the scroll no art, as Sumner's visit does when the hints' art is missing. */
    void setArt() { menu.setArt({}); }
    void open() {
        const HintMenuLabels labels{
            "HELP", {{"GENERAL", 39}, {"GUARDIANS", 40}}, "SELECT", "PLAYER"};
        REQUIRE(menu.open(painter, {}, labels));
    }
    Owner& owner() { return menu; }
    void advance() { (void)menu.update(device, {}, 1); }
    bool clear() {
        menu.close();
        return true;
    }
    bool rebind() {
        menu.close();
        setArt();
        open();
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** A promotion owed to a character who reached the thirtieth level, its lead-in counting
 * down. */
struct TowerPromotionRow {
    using Owner = TowerPromotion;
    static constexpr std::string_view kName = "TowerPromotion";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    std::array<PlayerRuntime, 1> party;
    const MessageTable strings;
    TowerPromotion promotion;

    TowerPromotionRow() {
        CharacterSave save;
        save.progress().experience = levelExperience(30);
        save.progress().promotedLevel = 29;
        party[0].actor.spawn(0, save, nullptr, Vec3{0}, 0);
        begin();
    }
    void begin() {
        promotion.begin(party, strings);
        REQUIRE(promotion.active());
    }
    Owner& owner() { return promotion; }
    void advance() { (void)promotion.update(1, false); }
    bool clear() {
        promotion.clear();
        return true;
    }
    bool rebind() {
        promotion.clear();
        begin();
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** A new runestone's ceremony over the world level, under way. */
struct TowerRelicsRow {
    using Owner = TowerRelics;
    static constexpr std::string_view kName = "TowerRelics";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    LevelWorld world;
    std::array<Relics, 1> party;
    const MessageTable strings;
    TowerRelics relics;
    NativeStateResources registry;

    TowerRelicsRow() {
        REQUIRE(world.load(device, worldStageRoot(), worldStageRef()));
        test::registerStage(registry, world, device);
        party[0].addRune(0);
        party[0].addShard(1);
        begin();
    }
    void begin() {
        relics.begin(party, strings);
        relics.bind(device, world, {});
    }
    Owner& owner() { return relics; }
    void advance() {
        relics.capturePresentation();
        (void)relics.update(127, 0.4f, false);
        relics.animate(0.4f);
    }
    bool clear() {
        relics.clear();
        return true;
    }
    bool rebind() {
        relics.clear();
        begin();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

static_assert(LocalOwner<SumnerFigureRow::Owner>);
static_assert(LocalOwner<SumnerHintsRow::Owner>);
static_assert(LocalOwner<SumnerVisitRow::Owner>);
static_assert(LocalOwner<HintMenuRow::Owner>);
static_assert(LocalOwner<TowerPromotionRow::Owner>);
static_assert(LocalOwner<TowerRelicsRow::Owner>);

TEST_CASE("the tower's snapshot owners keep their admission rules", "[local-owner-contract]") {
    test::checkLocalOwner<SumnerFigureRow>();
    test::checkLocalOwner<SumnerHintsRow>();
    test::checkLocalOwner<SumnerVisitRow>();
    test::checkLocalOwner<HintMenuRow>();
    test::checkLocalOwner<TowerPromotionRow>();
    test::checkLocalOwner<TowerRelicsRow>();
    // The hint scroll opened anew without its art set again.
    CHECK(test::askAfter<HintMenuRow>(&HintMenuRow::open) == Admitted);
}

// ---------------------------------------------------------------------------------------------
// A boss's fall: the legend item's flight and the sequence after it.

/** The legend items' trees, written once. */
const std::filesystem::path& legendArchive() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-legend");
        writeTextFile(dir / "body.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 1 0\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(dir / "objects.json", R"({"objects":[
            {"index":0,"name":"BODY","file":"body.obj","meshTriangles":1}]})");
        writeFile(dir / "skin.png", test::kTinyPng);
        writeTextFile(dir / "textures.json", R"({"defs":[],"bitmaps":[
            {"index":0,"name":"SEETHROUGH","file":"skin.png","width":2,"height":2,"flags":0},
            {"index":1,"name":"PARTICLE1_A","file":"skin.png","width":2,"height":2,"flags":0}]})");
        test::convertModelFixture(dir);
        std::string trees;
        for (const std::string_view tree :
             {"LEGENDHLD", "LEGENDPRJ", "LEGENDFX", "LEGENDFX2", "COMBO_SPH", "COMBO_BLU"}) {
            if (!trees.empty()) {
                trees += ',';
            }
            trees += std::format(R"({{"name":"{}","prefix":"","sequences":[],"nodes":[
                {{"name":"BODY","object":"BODY","type":0,"flags":0,"objectFlags":0,
                  "parent":-1,"position":[0,0,0]}}]}})",
                                 tree);
        }
        writeTextFile(dir / "animations.json", "{\"trees\":[" + trees + "]}");
        return dir;
    }();
    return kRoot;
}

/** A legend item brandished, thrown and flying at its target. */
struct LegendPresentationRow {
    using Owner = LegendPresentation;
    static constexpr std::string_view kName = "LegendPresentation";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Unasked,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    ItemArchive items;
    ItemArchive weapons;
    TextureSet shared;
    EffectTrees effects;
    LegendPresentation presentation{
        effects,
        {device, items, weapons, shared},
        {[](std::string_view) { return kNoSound; }, [](SoundHandle) {}}};
    /** The bearer holding the item up, and then letting it go. */
    const LegendPresentation::Bearer bearer{
        2, 1, Vec3{0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 8.0f, 0.0f}, true, false, false};
    const LegendPresentation::Bearer thrower{
        2, 1, Vec3{0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 8.0f, 0.0f}, true, true, true};
    const LegendPresentation::Target target{Vec3{20.0f, 0.0f, 1.0f}, 4.0f};
    NativeStateResources registry;
    s32 ticks = 0;

    LegendPresentationRow() {
        REQUIRE(items.load(legendArchive()));
        REQUIRE(registry.registerArchive(items, "legend"));
    }
    Owner& owner() { return presentation; }
    void advance() {
        ++ticks;
        if (ticks == 1) {
            presentation.show(LegendCue::Brandished, bearer.player, 2, 34, bearer);
            presentation.show(LegendCue::Thrown, bearer.player, 2, 34, bearer);
        }
        (void)presentation.update(0.1f, thrower, target);
    }
    bool clear() {
        presentation.clear();
        return true;
    }
    static bool rebind() { return false; }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** The world level's boss fallen, its wizard's visit under way for a party of one. */
struct BossSequenceRow {
    using Owner = BossSequence;
    static constexpr std::string_view kName = "BossSequence";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    TextureSet textures;
    EffectTrees effects;
    LevelSoundscape audio;
    const Bosses bosses{};
    std::array<PlayerRuntime, 1> players;
    const MessageTable strings;
    BossSequence sequence;
    NativeStateResources registry;
    s32 ticks = 0;

    BossSequenceRow() {
        REQUIRE(world.load(device, worldStageRoot(), worldStageRef()));
        test::registerStage(registry, world, device);
        players[0].actor.spawn(0, {}, nullptr, {0, 0, 4}, 0);
        bind();
    }
    ~BossSequenceRow() { sequence.clear(); }
    BossSequenceRow(const BossSequenceRow&) = delete;
    BossSequenceRow& operator=(const BossSequenceRow&) = delete;
    BossSequenceRow(BossSequenceRow&&) = delete;
    BossSequenceRow& operator=(BossSequenceRow&&) = delete;
    void bind() { sequence.bind({device, world, weapons, textures, effects, audio}); }
    Owner& owner() { return sequence; }
    void advance() {
        ++ticks;
        if (ticks == 1) {
            sequence.fallen({1, 0, 1}, bosses, players);
        }
        (void)sequence.advanceVictory(1, kStep, players, strings);
    }
    bool clear() {
        sequence.clear();
        return true;
    }
    bool rebind() {
        sequence.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

static_assert(LocalOwner<LegendPresentationRow::Owner>);
static_assert(LocalOwner<BossSequenceRow::Owner>);

TEST_CASE("the boss presentations' snapshot owners keep their admission rules",
          "[local-owner-contract]") {
    test::checkLocalOwner<LegendPresentationRow>();
    test::checkLocalOwner<BossSequenceRow>();
}

// ---------------------------------------------------------------------------------------------
// What a level sounds like, its effects, and the mechanics laid over it.

/** A speech queue without an output, held and let go in turn. */
struct VoiceQueueRow {
    using Owner = VoiceQueue;
    static constexpr std::string_view kName = "VoiceQueue";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    VoiceQueue queue;

    VoiceQueueRow() { queue.bind(nullptr); }
    Owner& owner() { return queue; }
    void advance() {
        queue.hold(!queue.held());
        queue.update(kStep);
    }
    bool clear() {
        queue.clear();
        return true;
    }
    bool rebind() {
        queue.clear();
        queue.bind(nullptr);
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** A level's sound folder with nothing in it, made once. */
const std::filesystem::path& silentStageRoot() {
    static const std::filesystem::path kRoot = test::scratchDirectory("local-owner-silence");
    return kRoot;
}

/** A silent level's soundscape, its music ducking further each step. */
struct LevelSoundscapeRow {
    using Owner = LevelSoundscape;
    static constexpr std::string_view kName = "LevelSoundscape";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    const LevelAudioInfo info{.bank = {}, .stream = {}, .areas = 3};
    LevelSoundscape soundscape;
    s32 ticks = 0;

    LevelSoundscapeRow() { open(); }
    ~LevelSoundscapeRow() { soundscape.close(); }
    LevelSoundscapeRow(const LevelSoundscapeRow&) = delete;
    LevelSoundscapeRow& operator=(const LevelSoundscapeRow&) = delete;
    LevelSoundscapeRow(LevelSoundscapeRow&&) = delete;
    LevelSoundscapeRow& operator=(LevelSoundscapeRow&&) = delete;
    void open() { soundscape.open(silentStageRoot(), nullptr, &info); }
    Owner& owner() { return soundscape; }
    void advance() {
        ++ticks;
        soundscape.duckMusic(0.125f, 1.0f / static_cast<f32>(ticks + 1));
    }
    bool clear() {
        soundscape.close();
        return true;
    }
    bool rebind() {
        soundscape.close();
        open();
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** The flights' trees, written once. */
const std::filesystem::path& flightArchive() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-flights");
        writeTriangleArchive(dir, R"(
          {"name":"WHITE","file":"white.png","width":2,"height":2},
          {"name":"WHITE1","file":"white.png","width":2,"height":2})",
                             R"({
          "textureAnimations":[{"name":"WHITE","texture":0,"source":0,"frames":2,"rate":1}],
          "particles":[{"enables":16928,"texture":"WHITE","particleLife":[1,0],
            "rate":[30,30,30,30],"flags":128,"flagMask":128}],
          "trees":[
            {"name":"FLIGHT","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
              {"name":"EMITTER","parent":0,"position":[0,1,0],"particle":0}],
             "sequences":[{"name":"ACTIVE","frames":9,"frameRate":30,
               "tracks":[{"node":0,"flags":32,"frames":[0,8],"values":[0,2]}]}]}]})");
        return dir;
    }();
    return kRoot;
}

/** A persistent flight playing on. */
struct EffectTreesRow {
    using Owner = EffectTrees;
    static constexpr std::string_view kName = "EffectTrees";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Unasked,
                                            .movedInto = Refused,
                                            .movedFrom = Refused,
                                            .assignedInto = Refused,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    ItemArchive archive;
    EffectTrees effects;
    NativeStateResources registry;

    EffectTreesRow() {
        REQUIRE(archive.load(flightArchive()));
        REQUIRE(registry.registerArchive(archive, "effects/source"));
        REQUIRE(registry.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
        EffectTrees::Setting setting;
        setting.persistent = true;
        REQUIRE(effects.startSet(device, archive, "FLIGHT", {3, 4, 5}, setting) != 0);
    }
    Owner& owner() { return effects; }
    void advance() {
        effects.capturePresentation();
        effects.update(1.0f / 30);
    }
    bool clear() {
        effects.clear();
        return true;
    }
    static bool rebind() { return false; }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** The mechanics of a catalog of one, switched on and stepped. */
struct MechanicsFacadeRow {
    using Owner = MechanicsFacade;
    static constexpr std::string_view kName = "MechanicsFacade";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    MechanicCatalog catalog;
    MechanicsFacade facade;

    MechanicsFacadeRow() {
        REQUIRE(catalog.add(test::FakeMechanic::descriptor("alpha")));
        open();
    }
    ~MechanicsFacadeRow() { facade.close(); }
    MechanicsFacadeRow(const MechanicsFacadeRow&) = delete;
    MechanicsFacadeRow& operator=(const MechanicsFacadeRow&) = delete;
    MechanicsFacadeRow(MechanicsFacadeRow&&) = delete;
    MechanicsFacadeRow& operator=(MechanicsFacadeRow&&) = delete;
    void open() {
        facade.open(&catalog, {});
        GameOptionsConfig on;
        on.mechanics["alpha"] = true;
        facade.configure(on);
    }
    Owner& owner() { return facade; }
    void advance() { facade.step({.seconds = 0.25f}); }
    bool clear() {
        facade.close();
        return true;
    }
    bool rebind() {
        facade.close();
        open();
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

static_assert(LocalOwner<VoiceQueueRow::Owner>);
static_assert(LocalOwner<LevelSoundscapeRow::Owner>);
static_assert(LocalOwner<EffectTreesRow::Owner>);
static_assert(LocalOwner<MechanicsFacadeRow::Owner>);

TEST_CASE("the audio, effects and mechanics snapshot owners keep their admission rules",
          "[local-owner-contract]") {
    test::checkLocalOwner<VoiceQueueRow>();
    test::checkLocalOwner<LevelSoundscapeRow>();
    test::checkLocalOwner<EffectTreesRow>();
    test::checkLocalOwner<MechanicsFacadeRow>();
}

// ---------------------------------------------------------------------------------------------
// The party: strikes, the arsenal, the attacks, missiles, figures and their companions.

/** A turbo burst started and playing out. */
struct MoveStrikesRow {
    using Owner = MoveStrikes;
    static constexpr std::string_view kName = "MoveStrikes";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Unasked,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    MoveStrikes strikes;
    s32 ticks = 0;

    Owner& owner() { return strikes; }
    void advance() {
        ++ticks;
        if (ticks == 1) {
            MoveStrike burst;
            burst.type = MoveStrike::kBursts;
            burst.delay = 0.1f;
            burst.radius = 12;
            burst.amount = 50;
            (void)strikes.start(burst, 0, {2, 3, 4}, {0, 0, 1}, 10, 0.75f);
        }
        (void)strikes.update(1.0f / 30, nullptr);
    }
    bool clear() {
        strikes.clear();
        return true;
    }
    static bool rebind() { return false; }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** What the attack scene's owners borrow, named in a digest. */
void registerAttackScene(NativeStateResources& registry, const test::AttackSceneFixture& scene) {
    REQUIRE(registry.registerBuiltinTexture(scene.device.whiteTexture(), "opaque-white"));
    if (scene.weapons.loaded()) {
        REQUIRE(registry.registerArchive(scene.weapons, "weapons"));
    }
    REQUIRE(registry.registerLayout(scene.world.layout(), "stage"));
    REQUIRE(registry.registerCollision(scene.world.collision(), "stage"));
}

/** One player's arsenal without artwork, cycling the unspecified potion's colour. */
struct PlayerArsenalRow {
    using Owner = PlayerArsenal;
    static constexpr std::string_view kName = "PlayerArsenal";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::AttackSceneFixture scene{false};
    NativeStateResources registry;

    PlayerArsenalRow() { registerAttackScene(registry, scene); }
    Owner& owner() { return scene.arsenal; }
    void advance() { (void)scene.arsenal.resolvePotionKind(0); }
    bool clear() {
        scene.arsenal.clear();
        return true;
    }
    bool rebind() {
        scene.arsenal.clear();
        scene.arsenal.bind({scene.device,
                            scene.classes,
                            scene.weapons,
                            scene.world.collision(),
                            scene.effects,
                            scene.audio,
                            nullptr,
                            {},
                            false,
                            false,
                            nullptr});
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** One player's attacks, a potion shattering at a new place each step. */
struct PlayerAttacksRow {
    using Owner = PlayerAttacks;
    static constexpr std::string_view kName = "PlayerAttacks";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::AttackSceneFixture scene;
    NativeStateResources registry;
    s32 ticks = 0;

    PlayerAttacksRow() { registerAttackScene(registry, scene); }
    Owner& owner() { return scene.attacks; }
    void advance() {
        ++ticks;
        scene.attacks.shatterPotion(0, {static_cast<f32>(ticks), 2, 3});
        scene.attacks.updateProjectiles(kStep, scene.players, scene.targets);
        scene.effects.update(kStep);
    }
    bool clear() {
        scene.attacks.clear();
        return true;
    }
    bool rebind() {
        scene.attacks.clear();
        scene.attacks.bind({scene.device, scene.classes, scene.world, scene.weapons, scene.effects,
                            scene.audio, nullptr, scene.arsenal, scene.dimmer});
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** A pool of player missiles, one in flight. */
struct PlayerMissilesRow {
    using Owner = PlayerMissiles;
    static constexpr std::string_view kName = "PlayerMissiles";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::ProjectileFixture fixture;
    PlayerMissiles missiles;
    NativeStateResources registry;
    s32 ticks = 0;

    PlayerMissilesRow() {
        missiles.bindVisuals(fixture.device);
        REQUIRE(registry.registerArchive(fixture.archive, "stage/projectiles"));
        REQUIRE(registry.registerBuiltinTexture(fixture.device.whiteTexture(), "opaque-white"));
    }
    Owner& owner() { return missiles; }
    void advance() {
        ++ticks;
        if (ticks == 1) {
            REQUIRE(missiles.launch(fixture.launch()));
        }
        missiles.update(kStep, nullptr);
    }
    bool clear() {
        missiles.clear();
        return true;
    }
    bool rebind() {
        missiles.clear();
        missiles.bindVisuals(fixture.device);
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** A familiar's tree, written once. */
const std::filesystem::path& familiarArchive() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-familiar");
        writeTriangleArchive(dir, R"({"name":"WHITE","file":"white.png","width":2,"height":2})",
                             R"({"trees":[{"name":"FAMILIAR1","nodes":[
            {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
            "sequences":[{"name":"READY","frames":8,"frameRate":30,
              "tracks":[{"node":0,"flags":32,"frames":[0,7],"values":[0,2]}]},
              {"name":"ATTACK","frames":8,"frameRate":30}]}]})");
        return dir;
    }();
    return kRoot;
}

/** The familiar earned at thirty, idling. */
struct PlayerFamiliarRow {
    using Owner = PlayerFamiliar;
    static constexpr std::string_view kName = "PlayerFamiliar";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Unasked,
                                            .restoredThenCleared = Unasked,
                                            .rebound = Refused,
                                            .movedInto = Admitted,
                                            .movedFrom = Refused,
                                            .assignedInto = Admitted,
                                            .copied = Admitted,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    ItemArchive archive;
    PlayerFamiliar familiar;
    NativeStateResources registry;

    PlayerFamiliarRow() {
        REQUIRE(archive.load(familiarArchive()));
        REQUIRE(registry.registerArchive(archive, "familiar"));
        bind();
    }
    void bind() { REQUIRE(familiar.bind(device, archive, 30, {0, 1, 0})); }
    Owner& owner() { return familiar; }
    void advance() {
        familiar.capturePresentation();
        familiar.update(1.0f / 30, false);
    }
    static bool clear() { return false; }
    bool rebind() {
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** The attack scene's warrior figure, walking. */
struct PlayerFigureRow {
    using Owner = PlayerFigure;
    static constexpr std::string_view kName = "PlayerFigure";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Unasked,
                                            .restoredThenCleared = Unasked,
                                            .rebound = Unasked,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    std::unique_ptr<PlayerFigure> figure;
    NativeStateResources registry;

    PlayerFigureRow() {
        figure =
            PlayerFigure::load(device, test::attackSceneAssets(), CharacterSave{}, Entrance::Skip);
        REQUIRE(figure);
        REQUIRE(registry.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
    }
    Owner& owner() { return *figure; }
    void advance() {
        figure->capturePresentation();
        figure->animate(0.5f, 1, kStep);
    }
    static bool clear() { return false; }
    static bool rebind() { return false; }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** Pojo's tree, written once. */
const std::filesystem::path& pojoArchive() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-pojo");
        std::string sequences;
        for (const std::string_view name : {"READY", "RUN", "ATTACK", "ATTPWR", "HIT", "DEATH"}) {
            if (!sequences.empty()) {
                sequences += ',';
            }
            sequences += std::format(R"({{"name":"{}","frames":8,"frameRate":30,
              "tracks":[{{"node":0,"flags":32,"frames":[0,7],"values":[0,2]}}]}})",
                                     name);
        }
        writeTriangleArchive(dir, R"({"name":"WHITE","file":"white.png","width":2,"height":2})",
                             R"({"trees":[{"name":"POJO","nodes":[
            {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],"sequences":[)" +
                                 sequences + "]}]}");
        return dir;
    }();
    return kRoot;
}

/** Pojo brought up by his powerup and running with the body. */
struct PowerupCompanionRow {
    using Owner = PowerupCompanion;
    static constexpr std::string_view kName = "PowerupCompanion";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Admitted,
                                            .restoredThenCleared = Admitted,
                                            .rebound = Unasked,
                                            .movedInto = Admitted,
                                            .movedFrom = Refused,
                                            .assignedInto = Admitted,
                                            .copied = Admitted,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    ItemArchive powerups;
    PowerupCompanion companion;
    NativeStateResources registry;

    PowerupCompanionRow() {
        REQUIRE(powerups.load(pojoArchive()));
        REQUIRE(registry.registerArchive(powerups, "powerups"));
        companion.choose(device, PowerupCompanion::Kind::Pojo, powerups, nullptr);
        REQUIRE(companion.shown());
    }
    Owner& owner() { return companion; }
    void advance() {
        companion.capturePresentation();
        companion.update(1.0f / 30, PlayerAnimator::Action::Run1, false, false);
    }
    bool clear() {
        companion.clear();
        return true;
    }
    static bool rebind() { return false; }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

static_assert(LocalOwner<MoveStrikesRow::Owner>);
static_assert(LocalOwner<PlayerArsenalRow::Owner>);
static_assert(LocalOwner<PlayerAttacksRow::Owner>);
static_assert(LocalOwner<PlayerMissilesRow::Owner>);
static_assert(LocalOwner<PlayerFamiliarRow::Owner>);
static_assert(LocalOwner<PlayerFigureRow::Owner>);
static_assert(LocalOwner<PowerupCompanionRow::Owner>);

TEST_CASE("the party's snapshot owners keep their admission rules", "[local-owner-contract]") {
    test::checkLocalOwner<MoveStrikesRow>();
    test::checkLocalOwner<PlayerArsenalRow>();
    test::checkLocalOwner<PlayerAttacksRow>();
    test::checkLocalOwner<PlayerMissilesRow>();
    test::checkLocalOwner<PlayerFamiliarRow>();
    test::checkLocalOwner<PlayerFigureRow>();
    test::checkLocalOwner<PowerupCompanionRow>();
}

// ---------------------------------------------------------------------------------------------
// The opponents: placements, statues, missiles, generators, the swarm, the great ones, the
// bosses and the level's opponents together.

/** Two grunts placed far off, watched from a camera whose attention wanders. */
struct PlacedEnemiesRow {
    using Owner = PlacedEnemies;
    static constexpr std::string_view kName = "PlacedEnemies";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    PlacedEnemies placed;
    s32 ticks = 0;

    PlacedEnemiesRow() { bind(); }
    void bind() {
        PlacedEnemies::Placement record;
        record.kind = kGruntKind;
        record.spawn.position = {0, 0, 100};
        placed.bind({record, record});
    }
    Owner& owner() { return placed; }
    void advance() {
        ++ticks;
        placed.observe(ViewVolume{}, Vec3{static_cast<f32>(ticks), 0, 0});
    }
    bool clear() {
        placed.clear();
        return true;
    }
    bool rebind() {
        placed.clear();
        bind();
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** The golem statue's tree, written once. */
const std::filesystem::path& statueArchive() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-statues");
        std::filesystem::create_directories(dir / "models");
        writeTextFile(dir / "models/body.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(dir / "objects.json", R"({"objects":[
          {"name":"STONE","file":"models/body.obj"}]})");
        writeFile(dir / "skin.png", test::kTinyPng);
        writeTextFile(dir / "textures.json", R"({"bitmaps":[
          {"name":"SKIN","file":"skin.png","width":2,"height":2},
          {"name":"GLOW","file":"skin.png","width":2,"height":2}]})");
        test::convertModelFixture(dir);
        writeTextFile(dir / "animations.json", R"({
          "textureAnimations":[{"name":"SKIN","texture":0,"source":0,"frames":2,"rate":3}],
          "particles":[{"enables":16928,"texture":"SKIN","particleLife":[1,0],
                        "rate":[30,30,30,30],"flags":128,"flagMask":128}],
          "trees":[{"name":"GOL_STATUE","nodes":[
            {"name":"STONE","object":"STONE","parent":-1,"position":[0,0,0]},
            {"name":"SMOKE","particle":0,"parent":0,"position":[0,2,0]}],
            "sequences":[{"name":"IDLE","frames":0,"frameRate":30},
                         {"name":"ACTIVE","frames":10,"frameRate":30}]}]})");
        return dir;
    }();
    return kRoot;
}

/** Two golem statues, the first woken and rising. */
struct CritterStatuesRow {
    using Owner = CritterStatues;
    static constexpr std::string_view kName = "CritterStatues";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    ItemArchive archive;
    CritterStatues statues;
    NativeStateResources registry;

    CritterStatuesRow() {
        REQUIRE(archive.load(statueArchive()));
        REQUIRE(registry.registerArchive(archive, "stage/statues/golem"));
        add();
    }
    void add() {
        statues.setContactGridBounds({-320, 0, -320}, {320, 0, 320});
        for (s32 index = 0; index < 2; ++index) {
            CritterStatues::Placement placement;
            placement.kind = CombatantKind::Golem;
            placement.form = "STONE";
            placement.instance.info = 4;
            placement.instance.name = "PLACEMENT";
            placement.instance.position = {static_cast<f32>(index * 30), 2, 4};
            placement.radius = 3;
            placement.height = 5;
            placement.viewRadius = 10;
            placement.sight = 12;
            REQUIRE(statues.add(device, archive, placement, nullptr));
        }
    }
    Owner& owner() { return statues; }
    void advance() {
        statues.wake(0);
        statues.update(1, kStep, [](const Vec3&, f32) { return true; });
    }
    bool clear() {
        statues.clear();
        return true;
    }
    bool rebind() {
        statues.clear();
        add();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(registry, owner);
    }
};

/** The swarm's missiles, one bolt in flight. */
struct EnemyMissilesRow {
    using Owner = EnemyMissiles;
    static constexpr std::string_view kName = "EnemyMissiles";
    static constexpr LocalOwnerRules kRules{.foreign = RefusedKeepingDigest,
                                            .blank = RefusedKeepingDigest,
                                            .cleared = RefusedKeepingDigest,
                                            .restoredThenCleared = RefusedKeepingDigest,
                                            .rebound = Unasked,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    EnemyMissiles missiles{71};
    s32 ticks = 0;

    Owner& owner() { return missiles; }
    void advance() {
        ++ticks;
        if (ticks == 1) {
            missiles.launch(EnemyMissileKind::bolt(12, 25, 0.5f), {0, 3, 0}, {0, 3, 40}, 1, nullptr,
                            4);
        }
        missiles.update(kStep, nullptr, {});
    }
    bool clear() {
        missiles.clear();
        return true;
    }
    static bool rebind() { return false; }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
};

/** A great one's shot's tree and its critter data, written once. */
const std::filesystem::path& combatShotArchive() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-combat-shots");
        writeTextFile(dir / "critter.json",
                      R"({"name":"TEST","types":[{"moveCount":1}],"descriptors":[{}],"moves":[{}],
          "damages":[{"type":1,"flags":32,"behaviorFlags":9,"radius":0.5,"damage":12,
            "minSpeed":30,"maxSpeed":30,"sfxIndex":0,"sfx":2}],
          "sounds":[{"name":"SHOT","flags":8}]})");
        writeTriangleArchive(dir, R"({"name":"WHITE","file":"white.png","width":2,"height":2})",
                             R"({"trees":[
          {"name":"SHOT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],
           "sequences":[{"name":"BIRTH","frames":3,"frameRate":30}]}]})");
        return dir;
    }();
    return kRoot;
}

/** The great ones' shots, one in flight. */
struct CombatantProjectilesRow {
    using Owner = CombatantProjectiles;
    static constexpr std::string_view kName = "CombatantProjectiles";
    static constexpr LocalOwnerRules kRules{.foreign = RefusedKeepingDigest,
                                            .blank = RefusedKeepingDigest,
                                            .cleared = RefusedKeepingDigest,
                                            .restoredThenCleared = RefusedKeepingDigest,
                                            .rebound = Unasked,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    ItemArchive archive;
    CritterData data;
    EffectTrees effects;
    CombatantProjectiles shots;
    s32 ticks = 0;

    CombatantProjectilesRow() {
        REQUIRE(archive.load(combatShotArchive()));
        REQUIRE(data.load(combatShotArchive() / "critter.json"));
    }
    ~CombatantProjectilesRow() { shots.clear(effects); }
    CombatantProjectilesRow(const CombatantProjectilesRow&) = delete;
    CombatantProjectilesRow& operator=(const CombatantProjectilesRow&) = delete;
    CombatantProjectilesRow(CombatantProjectilesRow&&) = delete;
    CombatantProjectilesRow& operator=(CombatantProjectilesRow&&) = delete;
    Owner& owner() { return shots; }
    void advance() {
        ++ticks;
        if (ticks == 1) {
            CombatShot shot;
            shot.data = &data;
            shot.critter = 11;
            shot.damageIndex = 0;
            shot.origin = {0, 3, 0};
            shot.target = Vec3{0, 3, 30};
            shots.launch(shot, archive, device, effects, {});
        }
        effects.capturePresentation();
        effects.update(kStep);
        shots.update(kStep, nullptr, {}, device, effects, {});
    }
    bool clear() {
        shots.clear(effects);
        return true;
    }
    static bool rebind() { return false; }
    /** Names the shot's critter data and archive, as a stage's registry would. */
    static bool name(NativeStateDigest& digest, std::type_index type, const void* address,
                     void* context) {
        const auto& row = *static_cast<const CombatantProjectilesRow*>(context);
        if (type == typeid(CritterData) && address == &row.data) {
            digest.string("stage/combat-shots/spec");
            return true;
        }
        if (type == typeid(ItemArchive) && address == &row.archive) {
            digest.string("stage/combat-shots/archive");
            return true;
        }
        return false;
    }
    NativeStateDigest::Resources resources(const Owner&) { return {this, name}; }
};

/** A grunt's archive, its generators' bodies and a level standing two generators, written
 * once. */
const std::filesystem::path& swarmStageRoot() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-swarm");
        const auto archive = dir / "MONSTERS/GRU";
        std::filesystem::create_directories(archive);
        writeTextFile(archive / "body.obj",
                      "v 0 0 1\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(archive / "objects.json", R"({"objects":[
          {"name":"BODY","file":"body.obj"},
          {"name":"GEN_GRU0","file":"body.obj"},
          {"name":"GEN_GRU1","file":"body.obj"},
          {"name":"GEN_GRU2","file":"body.obj"},
          {"name":"GEN_GRU3","file":"body.obj"}]})");
        writeFile(archive / "skin.png", test::kTinyPng);
        writeTextFile(archive / "textures.json", R"({"bitmaps":[
          {"name":"SKIN","file":"skin.png","width":2,"height":2},
          {"name":"GLOW","file":"skin.png","width":2,"height":2}]})");
        test::convertModelFixture(archive);
        std::string trees;
        for (const auto* const name : {"GRU1", "GRU2", "GRU3"}) {
            if (!trees.empty()) {
                trees += ',';
            }
            trees += std::string{R"({"name":")"} + name + R"(","nodes":[
              {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
              "sequences":[{"name":"READY","frames":12,"frameRate":30},
                {"name":"WALK","frames":8,"frameRate":30,
                  "tracks":[{"node":0,"flags":32,"frames":[0,7],"values":[0,1]}]},
                {"name":"ATTACK1","frames":3,"frameRate":30,"repeats":false},
                {"name":"ATTACK1R","frames":3,"frameRate":30,"repeats":false},
                {"name":"HIT1","frames":3,"frameRate":30,"repeats":false},
                {"name":"HIT2","frames":3,"frameRate":30,"repeats":false},
                {"name":"GETUP","frames":3,"frameRate":30,"repeats":false},
                {"name":"DEATH","frames":3,"frameRate":30,"repeats":false}]})";
        }
        writeTextFile(archive / "animations.json",
                      R"({"textureAnimations":[{"name":"SKIN","texture":0,"source":0,
                        "frames":2,"rate":3}],"trees":[)" +
                          trees + "]}");
        writeTextFile(dir / "world.json", R"({
          "objects":[{"name":"GROUND","position":[0,0,0]}],
          "itemInfos":[{"type":3,"name":"GRU","radius":2,"height":5,
                         "collisionType":1,"hitPoints":10}],
          "itemInstances":[
            {"info":0,"minPlayers":1,"position":[0,0,0],"params":[2,0,7,0,2,0,1,0,0,0,0,0]},
            {"info":0,"minPlayers":2,"position":[30,0,0],"params":[3,0,7,0,2,0,1,0,0,0,0,0]}]})");
        return dir;
    }();
    return kRoot;
}

constexpr std::string_view kSwarmKey = "stage/local-owner/swarm";

/** The players the opponents see: one standing in front of the swarm. */
std::array<EnemyView, 1> opponentsParty() {
    return {EnemyView{.player = 0, .position = {0, 0, 25}}};
}

/** The level's floor the swarm walks on. */
WorldCollision swarmFloor() {
    CollisionTriangle lower;
    lower.vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, -200}, Vec3{200, 0, 200}};
    lower.normal = {0, 1, 0};
    lower.object = 0;
    CollisionTriangle upper = lower;
    upper.vertices = {Vec3{-200, 0, -200}, Vec3{200, 0, 200}, Vec3{-200, 0, 200}};
    WorldCollision floor;
    floor.build({lower, upper});
    return floor;
}

/** A swarm of one grunt, chasing the party. */
struct EnemiesRow {
    using Owner = Enemies;
    static constexpr std::string_view kName = "Enemies";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    Enemies enemies;
    NativeStateResources registry;

    EnemiesRow() { open(); }
    ~EnemiesRow() { enemies.close(); }
    EnemiesRow(const EnemiesRow&) = delete;
    EnemiesRow& operator=(const EnemiesRow&) = delete;
    EnemiesRow(EnemiesRow&&) = delete;
    EnemiesRow& operator=(EnemiesRow&&) = delete;
    void open() {
        enemies.open(device, swarmStageRoot(), nullptr, 8, {}, 752);
        REQUIRE(enemies.loadKind(kGruntKind));
        EnemySpawn spawn;
        spawn.placed = true;
        spawn.algorithm = kChaseWay;
        spawn.position = {0, 0, -8};
        REQUIRE(enemies.spawn(spawn, {}) == 0);
    }
    Owner& owner() { return enemies; }
    void advance() { enemies.update(1, kStep, opponentsParty()); }
    bool clear() {
        enemies.close();
        return true;
    }
    bool rebind() {
        enemies.close();
        open();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        (void)owner.registerNativeResources(registry, kSwarmKey);
        return registry.resources();
    }
};

/** The swarm level's two grunt generators, breeding. */
struct GeneratorsRow {
    using Owner = Generators;
    static constexpr std::string_view kName = "Generators";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    WorldCollision collision = swarmFloor();
    WorldLayout layout;
    Enemies enemies;
    Generators generators;
    NativeStateResources registry;

    GeneratorsRow() {
        REQUIRE(layout.load(swarmStageRoot()));
        enemies.open(device, swarmStageRoot(), &collision, 8, {}, 347);
        bind();
        REQUIRE(registry.registerCollision(collision, "stage/local-owner/world"));
    }
    ~GeneratorsRow() {
        generators.clear();
        enemies.close();
    }
    GeneratorsRow(const GeneratorsRow&) = delete;
    GeneratorsRow& operator=(const GeneratorsRow&) = delete;
    GeneratorsRow(GeneratorsRow&&) = delete;
    GeneratorsRow& operator=(GeneratorsRow&&) = delete;
    void bind() { REQUIRE(generators.bind(device, layout, enemies, &collision, {}, 1)); }
    Owner& owner() { return generators; }
    void advance() {
        generators.update(1, enemies, opponentsParty());
        enemies.update(1, kStep, opponentsParty(), generators.enemyObstacles());
    }
    bool clear() {
        generators.clear();
        return true;
    }
    bool rebind() {
        generators.clear();
        bind();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        (void)enemies.registerNativeResources(registry, kSwarmKey);
        return registered(registry, owner);
    }
};

/** A djinn's critter data and archive, written once. */
const std::filesystem::path& djinnStageRoot() {
    static const std::filesystem::path kRoot = [] {
        const auto dir = test::scratchDirectory("local-owner-djinn");
        const auto archive = dir / "MONSTERS/DJINN";
        std::filesystem::create_directories(dir / "critter");
        std::filesystem::create_directories(archive / "models");
        std::filesystem::create_directories(archive / "textures");
        writeTextFile(archive / "models/body.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
        writeTextFile(archive / "objects.json", R"({"objects":[
          {"index":0,"name":"BODY","file":"models/body.obj","meshTriangles":1}]})");
        writeFile(archive / "textures/skin.png", test::kTinyPng);
        writeTextFile(archive / "textures.json", R"({"bitmaps":[
          {"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2,"flags":0}]})");
        test::convertModelFixture(archive);
        writeTextFile(archive / "animations.json", R"({"trees":[{"name":"DJINN",
          "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"READY","frames":1},{"name":"ROARATK","frames":25}]},
          {"name":"ROARFX","nodes":[],"sequences":[{"name":"PLAY","frames":30,"rate":30}]},
          {"name":"IMPACT","nodes":[],"sequences":[{"name":"PLAY","frames":30,"rate":30}]}]})");
        writeTextFile(dir / "critter/DJINN.json", R"({"name":"DJINN",
          "descriptors":[{"name":"djinn","prefix":"DJINN","type":4}],
          "types":[{"moveCount":2,"floorOffset":7,"vertDrift":10,"maxHealth":100}],
          "moves":[{"name":"READY","anim":"READY","type":32,"priority":1},
            {"name":"FOUNTAIN","anim":"ROARATK","type":136,"priority":10,"cooldown":10,
             "frameStart":0,"frameEnd":0,"damage0":0,
             "frameStart2":14,"frameEnd2":25,"damage1":1}],
          "damages":[{"type":8,"offset":[0,-100,0],"sfxIndex":0},
            {"type":8,"flags":32,"radius":3,"maxDistance":10,"damage":100,"sfxIndex":1}],
          "sounds":[{"name":"ROARFX","flags":66,"offset":[0,97,0]},
            {"name":"IMPACT","flags":66,"offset":[0,-2,0]}]})");
        return dir;
    }();
    return kRoot;
}

/** The djinn, turning towards a player and attacking. */
struct CombatantRow {
    using Owner = Combatant;
    static constexpr std::string_view kName = "Combatant";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Admitted,
                                            .restoredThenCleared = Admitted,
                                            .rebound = Admitted,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    std::unique_ptr<NativeStateResources> registry;

    CombatantRow() {
        fixture.open(device, djinnStageRoot(), nullptr, {}, 'G');
        spawn();
    }
    /** Loads the djinn's assets, spawns it over them and names them in a registry of their
     * own. */
    void spawn() {
        REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
        registry = std::make_unique<NativeStateResources>();
        REQUIRE(registry->registerCombatantAssets(fixture.assets, "stage/local-owner/djinn"));
    }
    /** Clears the djinn and spawns it over its assets loaded anew. */
    void reload() {
        fixture.actor.clear();
        spawn();
    }
    Owner& owner() { return fixture.actor; }
    void advance() {
        const std::array party{EnemyView{.player = 0, .position = {20, 0, 0}}};
        fixture.update(1, kStep, party);
    }
    bool clear() {
        fixture.actor.clear();
        return true;
    }
    bool rebind() {
        fixture.actor.clear();
        REQUIRE(fixture.actor.spawn(fixture.assets, 0, Vec3{0}, 0, nullptr, {}, 'G'));
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        return registered(*registry, owner);
    }
};

/** The great ones, opened over a level with none of their data, walking a lookout round. */
struct CrittersRow {
    using Owner = Critters;
    static constexpr std::string_view kName = "Critters";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    Critters critters;
    NativeStateResources registry;
    s32 ticks = 0;

    CrittersRow() { open(); }
    ~CrittersRow() { critters.close(); }
    CrittersRow(const CrittersRow&) = delete;
    CrittersRow& operator=(const CrittersRow&) = delete;
    CrittersRow(CrittersRow&&) = delete;
    CrittersRow& operator=(CrittersRow&&) = delete;
    void open() { critters.open(device, silentStageRoot(), nullptr, {}, 'G'); }
    Owner& owner() { return critters; }
    void advance() {
        ++ticks;
        const auto along = static_cast<f32>(ticks);
        critters.setLookouts({{Vec3{along, 0, 0}, Vec3{0, 0, along}}, {1, 65535}});
    }
    bool clear() {
        critters.close();
        return true;
    }
    bool rebind() {
        critters.close();
        open();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        (void)owner.registerNativeResources(registry, "stage/local-owner/critters");
        return registry.resources();
    }
};

/** The bosses, opened over a level with none of their data, an encounter's clock running. */
struct BossesRow {
    using Owner = Bosses;
    static constexpr std::string_view kName = "Bosses";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    Bosses bosses;
    NativeStateResources registry;

    BossesRow() { open(); }
    ~BossesRow() { bosses.close(); }
    BossesRow(const BossesRow&) = delete;
    BossesRow& operator=(const BossesRow&) = delete;
    BossesRow(BossesRow&&) = delete;
    BossesRow& operator=(BossesRow&&) = delete;
    void open() { bosses.open(device, silentStageRoot(), nullptr, {}, 'G'); }
    Owner& owner() { return bosses; }
    void advance() { bosses.arm(kStep); }
    bool clear() {
        bosses.close();
        return true;
    }
    bool rebind() {
        bosses.close();
        open();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        (void)owner.registerNativeResources(registry, "stage/local-owner/bosses");
        return registry.resources();
    }
};

/** A level's opponents over an empty world: a swarm of one grunt and its generator, the
 * grunt chasing a player. */
struct LevelOpponentsRow {
    using Owner = LevelOpponents;
    static constexpr std::string_view kName = "LevelOpponents";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    test::FakeRenderDevice device;
    LevelWorld world;
    WorldLayout layout;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    std::array<PlayerRuntime, 1> players;
    LevelOpponents opponents;
    NativeStateResources registry;

    LevelOpponentsRow() {
        REQUIRE(layout.load(swarmStageRoot()));
        players[0].actor.spawn(2, {}, nullptr, {0, 0, 20}, 0);
        open();
        test::registerStage(registry, world, device);
    }
    ~LevelOpponentsRow() { opponents.close(); }
    LevelOpponentsRow(const LevelOpponentsRow&) = delete;
    LevelOpponentsRow& operator=(const LevelOpponentsRow&) = delete;
    LevelOpponentsRow(LevelOpponentsRow&&) = delete;
    LevelOpponentsRow& operator=(LevelOpponentsRow&&) = delete;
    void open() {
        opponents.open({device, world, weapons, effects, audio, swarmStageRoot(), 1}, players);
        opponents.enemies().open(device, swarmStageRoot(), nullptr, 2, {.health = 1000}, 1);
        REQUIRE(opponents.enemies().loadKind(kGruntKind));
        REQUIRE(opponents.generators().bind(device, layout, opponents.enemies(), nullptr, {}, 1));
    }
    Owner& owner() { return opponents; }
    void advance() {
        opponents.generators().update(1, opponents.enemies(), opponentsParty());
        opponents.enemies().update(1, kStep, opponentsParty());
    }
    bool clear() {
        opponents.close();
        return true;
    }
    bool rebind() {
        opponents.close();
        open();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        (void)owner.enemies().registerNativeResources(registry, kSwarmKey);
        (void)owner.critters().registerNativeResources(registry, "stage/local-owner/critters");
        (void)owner.bosses().registerNativeResources(registry, "stage/local-owner/bosses");
        (void)owner.generators().registerNativeResources(registry);
        (void)owner.statues().registerNativeResources(registry);
        return registry.resources();
    }
};

static_assert(LocalOwner<PlacedEnemiesRow::Owner>);
static_assert(LocalOwner<CritterStatuesRow::Owner>);
static_assert(LocalOwner<EnemyMissilesRow::Owner>);
static_assert(LocalOwner<CombatantProjectilesRow::Owner>);
static_assert(LocalOwner<EnemiesRow::Owner>);
static_assert(LocalOwner<GeneratorsRow::Owner>);
static_assert(LocalOwner<CombatantRow::Owner>);
static_assert(LocalOwner<CrittersRow::Owner>);
static_assert(LocalOwner<BossesRow::Owner>);
static_assert(LocalOwner<LevelOpponentsRow::Owner>);

TEST_CASE("the opponents' snapshot owners keep their admission rules", "[local-owner-contract]") {
    test::checkLocalOwner<PlacedEnemiesRow>();
    test::checkLocalOwner<CritterStatuesRow>();
    test::checkLocalOwner<EnemyMissilesRow>();
    test::checkLocalOwner<CombatantProjectilesRow>();
    test::checkLocalOwner<EnemiesRow>();
    test::checkLocalOwner<GeneratorsRow>();
    test::checkLocalOwner<CombatantRow>();
    test::checkLocalOwner<CrittersRow>();
    test::checkLocalOwner<BossesRow>();
    test::checkLocalOwner<LevelOpponentsRow>();
    // The fighter spawned over its assets loaded anew; its row respawns it over the same ones.
    CHECK(test::askAfter<CombatantRow>(&CombatantRow::reload) == Refused);
}

// ---------------------------------------------------------------------------------------------
// Owners whose stock comes only from the game data: a real swarm, great one and boss, and the
// play scene over the tower.

/** The folder of the installed game data, or the test skipped without it. */
std::filesystem::path gameRoot() {
    return test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
}

/** Four players standing in front of a fighter. */
std::array<EnemyView, 4> fightParty() {
    std::array<EnemyView, 4> party;
    for (s32 seat = 0; seat < 4; ++seat) {
        auto& player = party[static_cast<usize>(seat)];
        player.player = seat;
        player.position = {static_cast<f32>(seat * 3 - 4), 0, 12};
    }
    return party;
}

/** A swarm of the game's own grunts, one chasing the party. */
struct SwarmStockRow {
    using Owner = Enemies;
    static constexpr std::string_view kName = "Enemies with the game's grunts";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    std::filesystem::path root = gameRoot();
    test::FakeRenderDevice device;
    Enemies enemies;
    NativeStateResources registry;

    SwarmStockRow() { open(); }
    ~SwarmStockRow() { enemies.close(); }
    SwarmStockRow(const SwarmStockRow&) = delete;
    SwarmStockRow& operator=(const SwarmStockRow&) = delete;
    SwarmStockRow(SwarmStockRow&&) = delete;
    SwarmStockRow& operator=(SwarmStockRow&&) = delete;
    void open() {
        enemies.open(device, root, nullptr, 8, {}, 752);
        REQUIRE(enemies.loadKind(kGruntKind));
        EnemySpawn spawn;
        spawn.placed = true;
        spawn.algorithm = kChaseWay;
        spawn.position = {0, 0, -8};
        REQUIRE(enemies.spawn(spawn, {}) == 0);
    }
    Owner& owner() { return enemies; }
    void advance() { enemies.update(1, kStep, fightParty()); }
    bool clear() {
        enemies.close();
        return true;
    }
    bool rebind() {
        enemies.close();
        open();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        (void)owner.registerNativeResources(registry, kSwarmKey);
        return registry.resources();
    }
};

/** The great ones with a golem of the game's standing, the party in front of it. */
struct GolemStockRow {
    using Owner = Critters;
    static constexpr std::string_view kName = "Critters with the game's golem";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    std::filesystem::path root = gameRoot();
    test::FakeRenderDevice device;
    Critters critters;
    NativeStateResources registry;

    GolemStockRow() { open(); }
    ~GolemStockRow() { critters.close(); }
    GolemStockRow(const GolemStockRow&) = delete;
    GolemStockRow& operator=(const GolemStockRow&) = delete;
    GolemStockRow(GolemStockRow&&) = delete;
    GolemStockRow& operator=(GolemStockRow&&) = delete;
    void open() {
        critters.open(device, root, nullptr, {}, 'G');
        REQUIRE(critters.spawn(CombatantKind::Golem, {0, 0, 0}, 0));
    }
    Owner& owner() { return critters; }
    void advance() { critters.update(1, kStep, fightParty()); }
    bool clear() {
        critters.close();
        return true;
    }
    bool rebind() {
        critters.close();
        open();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        (void)owner.registerNativeResources(registry, "stage/local-owner/critters");
        return registry.resources();
    }
};

/** A boss of the game's standing, asleep until the party comes near, the party in front. */
struct BossStockRow {
    using Owner = Bosses;
    static constexpr std::string_view kName = "Bosses with the game's boss";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    std::filesystem::path root = gameRoot();
    test::FakeRenderDevice device;
    Bosses bosses;
    NativeStateResources registry;

    BossStockRow() { open(); }
    ~BossStockRow() { bosses.close(); }
    BossStockRow(const BossStockRow&) = delete;
    BossStockRow& operator=(const BossStockRow&) = delete;
    BossStockRow(BossStockRow&&) = delete;
    BossStockRow& operator=(BossStockRow&&) = delete;
    void open() {
        bosses.open(device, root, nullptr, {}, 'G');
        REQUIRE(bosses.spawn(35, {}, 0, 30));
    }
    Owner& owner() { return bosses; }
    void advance() { bosses.update(1, kStep, fightParty()); }
    bool clear() {
        bosses.close();
        return true;
    }
    bool rebind() {
        bosses.close();
        open();
        return true;
    }
    NativeStateDigest::Resources resources(const Owner& owner) {
        (void)owner.registerNativeResources(registry, "stage/local-owner/bosses");
        return registry.resources();
    }
};

/** The tower opened for two seats, its party standing about. */
struct PlaySceneRow {
    using Owner = PlayScene;
    static constexpr std::string_view kName = "PlayScene";
    static constexpr LocalOwnerRules kRules{.foreign = Refused,
                                            .blank = Refused,
                                            .cleared = Refused,
                                            .restoredThenCleared = Refused,
                                            .rebound = Refused,
                                            .movedInto = Unasked,
                                            .movedFrom = Unasked,
                                            .assignedInto = Unasked,
                                            .copied = Unasked,
                                            .advances = true,
                                            .roundTrips = true};
    static constexpr s32 kSeats = 2;
    std::filesystem::path root = gameRoot();
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    StringTable strings;
    f32 stopTimeTotal = 0;
    LevelWorld world;
    GameServices context;
    std::vector<PartyMember> party;
    PlayScene scene;

    PlaySceneRow() {
        config.timing.tickRate = 60;
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        REQUIRE(world.load(device, root, LevelRef::tower()));
        context.config = &config;
        context.levels = &catalog;
        context.strings = &strings;
        context.unpackedRoot = root;
        for (s32 seat = 0; seat < kSeats; ++seat) {
            CharacterSave save;
            save.name = "TESTER";
            save.character = seat;
            save.color = seat;
            save.progress().health = 4000;
            party.push_back({seat, save, static_cast<usize>(seat), false, 100});
        }
        open();
    }
    ~PlaySceneRow() { scene.close(); }
    PlaySceneRow(const PlaySceneRow&) = delete;
    PlaySceneRow& operator=(const PlaySceneRow&) = delete;
    PlaySceneRow(PlaySceneRow&&) = delete;
    PlaySceneRow& operator=(PlaySceneRow&&) = delete;
    void open() {
        PlayOptions options;
        options.welcome = false;
        REQUIRE(
            scene.open(device, context, world, party, options, {.stopTimeTotal = &stopTimeTotal}));
    }
    Owner& owner() { return scene; }
    void advance() {
        REQUIRE(scene.update(1.0 / 60.0, PlayScene::Inputs{}) == PlayOutcome::Running);
    }
    bool clear() {
        scene.close();
        return true;
    }
    bool rebind() {
        scene.close();
        open();
        return true;
    }
    static NativeStateDigest::Resources resources(const Owner&) { return {}; }
    /** The scene hashes itself over the resources it registers. */
    static std::optional<NativeStateDigest::Digest> liveDigest(PlayScene& owner) {
        return owner.nativeAgreementDigest();
    }
    static std::optional<NativeStateDigest::Digest>
    savedDigest(PlayScene& owner, const PlayScene::LocalSnapshot& saved) {
        return owner.nativeLocalDigest(saved);
    }
};

static_assert(LocalOwner<SwarmStockRow::Owner>);
static_assert(LocalOwner<GolemStockRow::Owner>);
static_assert(LocalOwner<BossStockRow::Owner>);
// The scene holds the roster rather than standing in it: it hashes itself through its
// agreement digest.
static_assert(!LocalOwner<PlaySceneRow::Owner>);

TEST_CASE("the snapshot owners of the game's own stock keep their admission rules",
          "[local-owner-contract][assets]") {
    test::checkLocalOwner<SwarmStockRow>();
    test::checkLocalOwner<GolemStockRow>();
    test::checkLocalOwner<BossStockRow>();
    test::checkLocalOwner<PlaySceneRow>();
}

} // namespace
