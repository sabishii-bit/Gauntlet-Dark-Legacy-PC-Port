#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"

#include "game/level/ImportedLevel.h"
#include "game/level/ImportedWorldObject.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/rollback/ConsoleAction.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/PlayedLevel.h"
#include "game/scenes/SceneDescription.h"
#include "game/scenes/SceneSelection.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Lines = std::vector<std::string>;
using Kind = SceneSelection::Kind;

constexpr f64 kStep = 1.0 / 60.0;
constexpr s32 kSceneryPiece = 4; ///< the one piece of the imported scenery

SceneSelection selected(Kind kind, s32 id) {
    return SceneSelection{.kind = kind, .id = id, .name = {}};
}

/** An imported level whose scenery is one hidden piece, as a Legends map tells its own. */
class OnePieceScenery final : public ImportedLevel {
public:
    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
    std::optional<ImportedWorldObject> object(s32 id) const override {
        if (id != kSceneryPiece) {
            return std::nullopt;
        }
        return ImportedWorldObject{.id = kSceneryPiece,
                                   .name = "BRIDGE_L",
                                   .position = {1.25f, -2.0f, 30.5f},
                                   .flags = 0x41,
                                   .drawFlags = 0x200,
                                   .triangles = 96,
                                   .visible = false};
    }
};

/** Stands in `level`, through the console, three grunts, a golem, a lion gargoyle, a rat
 * generator, a copy of the level's first object and one more of its first pickup. */
void standThings(test::PlayedLevel& level) {
    const std::vector<ConsoleAction> actions{
        ConsoleAction{0, "spawn gru 1 3"},
        ConsoleAction{0, "spawn golem"},
        ConsoleAction{0, "spawn gargoyle lion"},
        ConsoleAction{0, "spawn generator rat 2"},
        ConsoleAction{0, "spawn object " + level.world.layout().objects().front().name},
        ConsoleAction{0, "spawn item " + level.world.placedItems().item(0).name}};
    const auto step = level.scene.step(kStep, {}, actions);
    REQUIRE(step.replies.size() == actions.size());
    CHECK(step.replies[0].text == "Spawned 3 GRU at tier 1");
    CHECK(step.replies[1].text == "Spawned a golem");
    CHECK(step.replies[2].text == "Spawned a GAR_LION");
    CHECK(step.replies[3].text == "Stood a tier 2 RAT generator");
    CHECK(step.replies[4].text == "Stood G1POLYSET#0 of G1");
    CHECK(step.replies[5].text == "Dropped BANANNA");
}
} // namespace

TEST_CASE("a thing selected in a level is described by its kind", "[dev-console][assets]") {
    test::PlayedLevel level{"G1"};
    standThings(level);
    const auto describe = [&](Kind kind, s32 id) {
        return describeSelection(level.scene, level.world, selected(kind, id), &level.world);
    };

    CHECK(describe(Kind::Switch, 0) == Lines{"switch#0 BRIDGEPAD: switch 0 of G1",
                                             "at 54.7 17.8 -203.1; id 0; drives G1ELEV7; waiting",
                                             "activate $SELECTED presses it"});
    CHECK(describe(Kind::Switch, 2) ==
          Lines{"switch#2 BRIDGEPAD: switch 2 of G1",
                "at 83.8 12.9 -228.8; id 2; drives G1ELEV13; waiting, fired through another",
                "activate $SELECTED presses it"});
    CHECK(describe(Kind::Switch, 34) == Lines{"switch#34 names no switch of G1"});
    CHECK(describe(Kind::Switch, -1) == Lines{"switch#-1 names no switch of G1"});

    CHECK(describe(Kind::Gate, 0) ==
          Lines{"gate#0 GATED: door 0 of G1", "at 24.5 42.8 -350.2; shut: a key opens it",
                "unlock $SELECTED opens it without a key; lock $SELECTED shuts it"});
    CHECK(describe(Kind::Gate, 2) == Lines{"gate#2 names no door of G1"});

    CHECK(describe(Kind::Chest, 0) ==
          Lines{"chest#0 CHEST: chest 0 of G1", "at 61.6 17.2 -138.0; shut: a key opens it",
                "unlock $SELECTED lets it open without a key; lock $SELECTED asks one"});
    CHECK(describe(Kind::Chest, 28) == Lines{"chest#28 names no chest of G1"});

    CHECK(describe(Kind::Object, 0) == Lines{"object#0 G1POLYSET#0: object 0 of G1",
                                             "at 49.2 27.9 -94.2; radius 8.5; parent none",
                                             "flags 0x2 prelit; object flags 0x0", "static; shown",
                                             "mesh: 4 triangles, 1 part: WATERFALL+6"});
    CHECK(describe(Kind::Object, 1) == Lines{"object#1 G1LOFTEDSU#000: object 1 of G1",
                                             "at 15.1 -4.6 -4.0; radius 0.0; parent none",
                                             "flags 0x2 prelit; object flags 0x0", "static; shown",
                                             "no mesh"});
    CHECK(describe(Kind::Object, 2) ==
          Lines{"object#2 G1#000: object 2 of G1",
                "at 15.1 -4.6 -4.0; radius 34.9; parent G1LOFTEDSU#000 (object 1)",
                "flags 0x2 prelit; object flags 0x0", "static; shown",
                "mesh: 40 triangles, 5 parts: GROUNDSIDE_01"});
    CHECK(describe(Kind::Object, 1360) == Lines{"object#1360 names no object of G1"});

    CHECK(describe(Kind::Enemy, 0) ==
          Lines{"enemy#0 GRU: tier 1, health 7", "at 21.0 0.0 -2.6; radius 1.5, height 6.0"});
    CHECK(describe(Kind::Enemy, 64) == Lines{"enemy#64 is no longer there"});

    CHECK(describe(Kind::Critter, 0) ==
          Lines{"critter#0 golem: health 300", "at 21.0 0.0 -2.6; radius 4.0"});
    CHECK(describe(Kind::Critter, 1) ==
          Lines{"critter#1 gargoyle LION: health 450", "at 21.0 0.0 -2.6; radius 3.0"});
    CHECK(describe(Kind::Critter, 2) == Lines{"critter#2 is no longer there"});

    CHECK(describe(Kind::Generator, 0) ==
          Lines{"generator#0 ZOM: tier 2, state 2 of 3, health 15", "at 111.2 10.1 -72.5"});
    CHECK(describe(Kind::Generator, 117) ==
          Lines{"generator#117 RAT: tier 2, state 2 of 3, health 15", "at 21.0 0.1 -2.6"});
    CHECK(describe(Kind::Generator, 118) == Lines{"generator#118 is no longer there"});

    CHECK(describe(Kind::Pickup, 0) ==
          Lines{"pickup#0 BANANNA: food, worth 50, unseen",
                "at 5.5 9.5 -112.2; radius 0.5, height 2.0; instance 239 of record 44"});
    CHECK(describe(Kind::Pickup, 141) ==
          Lines{"pickup#141 BANANNA: food, worth 50",
                "at 21.0 0.1 -2.6; radius 0.5, height 2.0; instance -1 of record 44"});
    CHECK(describe(Kind::Pickup, 142) == Lines{"pickup#142 is no longer there"});

    CHECK(describe(Kind::Player, 0) ==
          Lines{"player#0 TESTER: WAR of level 1, health 4000",
                "at 25.1 0.0 1.8; facing -137 degrees; radius 0.8, height 5.0"});
    CHECK(describe(Kind::Player, 1) ==
          Lines{"player#1 TESTER: VAL of level 1, health 4000",
                "at 23.6 0.0 3.2; facing -137 degrees; radius 0.8, height 5.0"});
    CHECK(describe(Kind::Player, 2) == Lines{"player#2 is no longer there"});

    CHECK(describe(Kind::Spawned, 0) ==
          Lines{"spawned#0 G1POLYSET#0: stood by the console", "at 21.0 0.0 -2.6; radius 8.5"});
    CHECK(describe(Kind::Spawned, 1) == Lines{"spawned#1 is no longer there"});
}

TEST_CASE("a shared scene's description binds no mesh", "[dev-console][assets]") {
    test::PlayedLevel level{"G1"};
    const auto describe = [&](Kind kind, s32 id, LevelWorld* meshes) {
        return describeSelection(level.scene, level.world, selected(kind, id), meshes);
    };
    // Without the world's models an object is described up to its mesh and no further.
    const Lines shared = describe(Kind::Object, 0, nullptr);
    CHECK(shared == Lines{"object#0 G1POLYSET#0: object 0 of G1",
                          "at 49.2 27.9 -94.2; radius 8.5; parent none",
                          "flags 0x2 prelit; object flags 0x0", "static; shown"});
    CHECK(describe(Kind::Object, 1, nullptr) == Lines{"object#1 G1LOFTEDSU#000: object 1 of G1",
                                                      "at 15.1 -4.6 -4.0; radius 0.0; parent none",
                                                      "flags 0x2 prelit; object flags 0x0",
                                                      "static; shown"});
    // With them the same lines come first, then the mesh.
    Lines offline = shared;
    offline.emplace_back("mesh: 4 triangles, 1 part: WATERFALL+6");
    CHECK(describe(Kind::Object, 0, &level.world) == offline);
    // Every other kind reads the same either way.
    for (const Kind kind : {Kind::Switch, Kind::Gate, Kind::Chest, Kind::Enemy, Kind::Critter,
                            Kind::Generator, Kind::Pickup, Kind::Player, Kind::Spawned}) {
        CAPTURE(SceneSelection::kindName(kind));
        CHECK(describe(kind, 0, nullptr) == describe(kind, 0, &level.world));
    }
}

TEST_CASE("with nothing in play every selection is gone or names nothing", "[dev-console]") {
    test::PlayedLevel level;
    REQUIRE_FALSE(level.scene.isOpen());
    for (LevelWorld* meshes : {static_cast<LevelWorld*>(nullptr), &level.world}) {
        const auto describe = [&](Kind kind, s32 id) {
            return describeSelection(level.scene, level.world, selected(kind, id), meshes);
        };
        CHECK(describe(Kind::Switch, 0) == Lines{"switch#0 names no switch of L1"});
        CHECK(describe(Kind::Gate, 0) == Lines{"gate#0 names no door of L1"});
        CHECK(describe(Kind::Chest, 0) == Lines{"chest#0 names no chest of L1"});
        CHECK(describe(Kind::Object, 0) == Lines{"object#0 names no object of L1"});
        CHECK(describe(Kind::Enemy, 0) == Lines{"enemy#0 is no longer there"});
        CHECK(describe(Kind::Critter, 0) == Lines{"critter#0 is no longer there"});
        CHECK(describe(Kind::Generator, 0) == Lines{"generator#0 is no longer there"});
        CHECK(describe(Kind::Pickup, 0) == Lines{"pickup#0 is no longer there"});
        CHECK(describe(Kind::Player, 0) == Lines{"player#0 is no longer there"});
        CHECK(describe(Kind::Spawned, 0) == Lines{"spawned#0 is no longer there"});
    }
}

TEST_CASE("an imported level's scenery is described as the level tells it", "[dev-console]") {
    test::PlayedLevel level;
    LevelRef ref;
    ref.realm = "Legends";
    ref.name = "n64:test";
    WorldCollision collision;
    collision.build(
        {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, -100}}, 0, 4}});
    auto layout = WorldLayout::fromDecoded({}, {}, {-100, 0, -100}, {100, 40, 100});
    const bool loaded =
        level.world.loadImported(level.device, level.root, ref, std::move(layout),
                                 std::move(collision), std::make_shared<OnePieceScenery>());
    REQUIRE(loaded);
    // The imported level's own word is taken, never a mesh of the world's models.
    for (LevelWorld* meshes : {static_cast<LevelWorld*>(nullptr), &level.world}) {
        const auto describe = [&](s32 id) {
            return describeSelection(level.scene, level.world, selected(Kind::Object, id), meshes);
        };
        CHECK(describe(kSceneryPiece) ==
              Lines{"object#4 BRIDGE_L: object 4 of n64:test", "at 1.2 -2.0 30.5",
                    "flags 0x41; draw flags 0x200; 96 triangles; hidden"});
        CHECK(describe(5) == Lines{"object#5 is no longer there"});
        CHECK(describe(0) == Lines{"object#0 is no longer there"});
    }
}
