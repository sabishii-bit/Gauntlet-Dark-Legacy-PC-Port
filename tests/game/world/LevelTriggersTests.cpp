#include <array>
#include <format>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/CritterStatues.h"
#include "game/players/PlayerActor.h"
#include "game/world/LevelCatalog.h"
#include "game/world/LevelTriggers.h"
#include "game/world/LevelWorld.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

struct SwitchFixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    WorldScene scene;
    WorldAnimator animator;
    LevelTriggers triggers;

    explicit SwitchFixture(
        std::string_view instances,
        std::string_view objects = R"({"name":"WALL","position":[0,10,0],"flags":4096})",
        s32 subtype = 24, std::string_view animations = "", f32 height = 0.0f,
        u32 collisionFlags = 0) {
        const auto dir = test::sampleLevel("switch-modes");
        writeTextFile(dir / "world.json",
                      std::format(R"({{
          "objects":[{}],
          "itemInfos":[{{"type":5,"subtype":{},"name":"BRIDGEPAD","radius":1,"height":{},"collisionFlags":{}}}],
          "itemInstances":[{}],"animations":[{}]}})",
                                  objects, subtype, height, collisionFlags, instances, animations));
        REQUIRE(layout.load(dir));
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(scene.build(layout, models, textures, device));
        animator.bind(layout);
        triggers.bind(layout, animator, nullptr);
        triggers.openMet({}, animator, scene, nullptr);
    }
    void step(f32 seconds, std::span<const TriggerVisitor> visitors = {}) {
        animator.step(seconds, scene);
        triggers.update(seconds, visitors, animator, scene, nullptr);
    }
    f32 height() const { return scene.worldTransform(0)[3].y; }
};

TEST_CASE("trigger contact uses authored height and the visitor's half height",
          "[triggers][temple-gates]") {
    SwitchFixture f(R"({"info":0,"position":[0,-5,0],"params":[0,0,0,0,0,255,0,0,0,0,0,0]})",
                    R"({"name":"WALL","position":[0,10,0]})", 24, "", 3.0f);
    std::array party{TriggerVisitor{.position = Vec3{0}, .height = 3.0f}};
    f.step(kStep, party);
    CHECK_FALSE(f.triggers.trigger(0).fired);
    party[0].height = 5.0f;
    f.step(kStep, party);
    CHECK(f.triggers.trigger(0).fired);
    party[0].position.y = 0.51f;
    f.step(kStep, party);
    CHECK_FALSE(f.triggers.trigger(0).fired);
    party[0].position = Vec3{3, 0, 0};
    f.step(kStep, party);
    CHECK_FALSE(f.triggers.trigger(0).fired);
}

TEST_CASE("a switch and its artwork follow the floor from its rest pose through motion",
          "[triggers][temple-gates][trigger-visibility]") {
    SwitchFixture f(R"({"info":0,"position":[1,15,0],"params":[0,0,0,0,0,255,0,0,0,0,0,0]})");
    CollisionTriangle floor;
    floor.object = 0;
    floor.objectFlags |= WorldObject::kAnimated;
    floor.normal = Vec3{0, 1, 0};
    floor.vertices = {Vec3{-10, 0, -10}, Vec3{10, 0, -10}, Vec3{0, 0, 10}};
    WorldCollision collision;
    collision.build({floor});
    collision.setMovingObjects(std::array<s32, 1>{0});
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, 30, 0}));
    f.triggers.bind(f.layout, f.animator, &collision);
    REQUIRE(f.triggers.trigger(0).floor == 0);
    CHECK(f.triggers.trigger(0).spot.y == Approx(30.1f));
    // An initial pose outside the floor probe must not prevent rest-pose attachment.
    CHECK((*collision.objectTransform(0))[3].y == Approx(30));
    const auto dir = test::sampleLevel("trigger-moving-art");
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"BRIDGEPAD",
      "nodes":[{"name":"WALL","object":"WALL","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"OFF","frames":1,"rate":30}]}]})");
    ItemArchive items;
    REQUIRE(items.load(dir));
    f.triggers.bindFigures(f.device, f.layout, items);
    f.triggers.draw(f.device, Mat4{1}, {});
    REQUIRE(f.device.draws.size() == 1);
    CHECK(f.device.draws.front().vertices.front().position.y == Approx(30.1f));
    std::array party{TriggerVisitor{.position = Vec3{1, 15, 0}}};
    f.triggers.update(kStep, party, f.animator, f.scene, &collision);
    CHECK_FALSE(f.triggers.trigger(0).fired);
    party[0].position = Vec3{1, 30, 0};
    f.triggers.update(kStep, party, f.animator, f.scene, &collision);
    CHECK(f.triggers.trigger(0).fired);
    const Mat4 moved =
        glm::rotate(glm::translate(Mat4{1}, Vec3{10, 4, 0}), glm::radians(90.0f), Vec3{0, 1, 0});
    collision.setObjectTransform(0, moved);
    f.triggers.update(kStep, party, f.animator, f.scene, &collision);
    CHECK_FALSE(f.triggers.trigger(0).fired);
    CHECK(f.triggers.trigger(0).spot.x == Approx(10));
    CHECK(f.triggers.trigger(0).spot.y == Approx(4.1f));
    CHECK(f.triggers.trigger(0).spot.z == Approx(-1));
    party[0].position = f.triggers.trigger(0).spot;
    f.triggers.update(kStep, party, f.animator, f.scene, &collision);
    CHECK(f.triggers.trigger(0).fired);
    f.device.draws.clear();
    f.triggers.draw(f.device, Mat4{1}, {});
    REQUIRE(f.device.draws.size() == 1);
    CHECK(f.device.draws.front().vertices.front().position.y == Approx(4.1f));
    f.triggers.clear();
}

TEST_CASE("floor-bound party pads require that floor while no-snap markers keep their height",
          "[triggers][platform-contact]") {
    const bool noSnap = GENERATE(false, true);
    SwitchFixture f(R"({"info":0,"position":[1,15,0],"params":[0,0,0,4,0,255,0,0,0,0,0,0]})",
                    R"({"name":"WALL","position":[0,10,0],"flags":4096})", 24, "", 3,
                    noSnap ? 1U : 0U);
    CollisionTriangle floor;
    floor.object = 0;
    floor.objectFlags |= WorldObject::kAnimated;
    floor.normal = Vec3{0, 1, 0};
    floor.vertices = {Vec3{-10, 0, -10}, Vec3{10, 0, -10}, Vec3{0, 0, 10}};
    WorldCollision collision;
    collision.build({floor});
    collision.setMovingObjects(std::array<s32, 1>{0});
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, 30, 0}));
    f.triggers.bind(f.layout, f.animator, &collision);
    const auto& pad = f.triggers.trigger(0);
    CHECK(pad.spot.y == Approx(noSnap ? 15 : 30.1f));
    CHECK(pad.floor == (noSnap ? -1 : 0));
    CHECK(((pad.flags & LevelTrigger::kOnTarget) != 0) == !noSnap);
    std::array visitors{TriggerVisitor{.position = pad.spot}};
    f.triggers.update(kStep, visitors, f.animator, f.scene, &collision);
    CHECK(pad.fired == noSnap);
    visitors[0].floorObject = 0;
    f.triggers.update(kStep, visitors, f.animator, f.scene, &collision);
    CHECK(pad.fired);
}

TEST_CASE("subtype 23 supplies a movement lesson only when it has a target", "[triggers][help]") {
    const SwitchFixture f(R"({"info":0,"position":[0,0,0],"params":[0,0,0,0,0,0,7,0,0,0,0,0]})",
                          R"({"name":"WALL","position":[0,10,0],"flags":4096})", 23);
    REQUIRE(f.triggers.trigger(0).movementLesson);
    const SwitchFixture missing(
        R"({"info":0,"position":[0,0,0],"params":[255,255,0,0,0,0,7,0,0,0,0,0]})",
        R"({"name":"WALL","position":[0,10,0],"flags":4096})", 23);
    REQUIRE_FALSE(missing.triggers.trigger(0).movementLesson);
}

TEST_CASE("special activation opens a chain and cannot be undone by an empty pad",
          "[triggers][tower-relics]") {
    SwitchFixture f(R"({"info":0,"position":[0,0,0],"params":[0,0,0,0,0,4,255,7,0,0,156,255]},
                        {"info":0,"position":[0,0,0],"params":[1,0,0,0,0,4,7,0,0,0,156,255]})",
                    R"({"name":"WALL","position":[0,10,0],"flags":4096},
                        {"name":"WALL","position":[0,10,0],"flags":4096})");
    f.triggers.activate(255, false, f.animator, f.scene, nullptr);
    REQUIRE(f.triggers.opened(0));
    REQUIRE(f.triggers.opened(1));
    REQUIRE_FALSE(f.triggers.settled(0));
    REQUIRE(f.triggers.takeOpenings().size() == 2);
    f.step(3);
    REQUIRE(f.triggers.settled(0));
    REQUIRE(f.triggers.settled(1));
    REQUIRE(f.height() == Approx(0));
    f.step(3);
    REQUIRE(f.height() == Approx(0));
    REQUIRE(f.triggers.trigger(0).fired);
    REQUIRE(f.triggers.trigger(1).fired);
    REQUIRE(f.triggers.takeCameraCues().empty());
}

TEST_CASE("switch camera cues occur on activation and expose target completion",
          "[triggers][switch-camera]") {
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,2,0,0,4,7,0,0,0,156,255]})");
    CHECK(f.triggers.takeCameraCues().empty());
    CHECK(f.triggers.settled(0));
    CHECK(f.triggers.settled(-1));
    const std::array party{TriggerVisitor{.position = Vec3{0}}};
    f.step(kStep, party);
    const auto cues = f.triggers.takeCameraCues();
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].id == 7);
    CHECK(cues[0].target == 0);
    CHECK_FALSE(f.triggers.settled(0));
    f.step(kStep, party);
    CHECK(f.triggers.takeCameraCues().empty());
    for (int frame = 0; frame < 300; ++frame) {
        f.step(kStep, party);
    }
    CHECK(f.triggers.settled(0));
    CHECK(f.triggers.takeCameraCues().empty());
}

TEST_CASE("shake triggers report an activation edge even without a camera marker",
          "[triggers][switch-camera][shake]") {
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,2,16,0,4,7,0,0,0,156,255]})");
    const std::array party{TriggerVisitor{.position = Vec3{0}}};
    f.step(kStep, party);
    const auto cues = f.triggers.takeCameraCues();
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0].shakes);
    f.step(kStep, party);
    REQUIRE(f.triggers.takeCameraCues().empty());
}

TEST_CASE("a pad flagged to wake a statue reports its spot once as it goes active",
          "[triggers][critter-statues]") {
    // The castle's pad at the gargoyle's feet: no target, opens once, wakes (flags 0x2002).
    SwitchFixture f(R"({"info":0,"position":[-75.25,0,32.5],
      "params":[254,255,2,32,12,0,10,0,0,0,0,0]})");
    REQUIRE(f.triggers.size() == 1);
    CHECK((f.triggers.trigger(0).flags & LevelTrigger::kWakesStatue) != 0);
    CHECK(f.triggers.takeWakes().empty());
    const std::array party{TriggerVisitor{.position = Vec3{-75.25f, 0, 32.5f}}};
    f.step(kStep, party);
    const auto wakes = f.triggers.takeWakes();
    REQUIRE(wakes.size() == 1);
    CHECK(wakes[0] == Vec3{-75.25f, 0, 32.5f});
    f.step(kStep, party);
    CHECK(f.triggers.takeWakes().empty());
    // Off it and back on, a pad that opens once has gone active once for good.
    f.step(kStep, {});
    f.step(kStep, party);
    CHECK(f.triggers.takeWakes().empty());
    CHECK(CritterStatues::activeTicks(85, 30) == 170); // the gargoyle's ACTIVE
    CHECK(CritterStatues::activeTicks(15, 60) == 60);  // the golem's
    CHECK(CritterStatues::activeTicks(0, 30) == 0);
}

TEST_CASE("the tower's lifts are opened at once by trigger id, the pads left alone",
          "[game][world][triggers][tower-access]") {
    // A lift pad with the tower's id 104, its target closed at 0 and open at -10.
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,2,0,0,4,104,0,0,0,156,255]})");
    f.triggers.takeOpenings();
    f.triggers.openAtOnce(std::array<s32, 1>{7}, f.animator, f.scene, nullptr);
    CHECK_FALSE(f.triggers.opened(0));
    CHECK(f.triggers.takeOpenings().empty());
    CHECK(f.height() == Approx(10.0f)); // where the layout put it
    f.triggers.openAtOnce(std::array<s32, 2>{104, 199}, f.animator, f.scene, nullptr);
    CHECK(f.triggers.opened(0));
    CHECK(f.triggers.settled(0));
    CHECK(f.height() == Approx(0.0f)); // ten down at once, its open height
    CHECK_FALSE(f.triggers.trigger(0).fired);
    const auto openings = f.triggers.takeOpenings();
    REQUIRE(openings.size() == 1);
    CHECK(openings[0].target == 0);
    CHECK(openings[0].atOnce);
    CHECK(openings[0].sound == 4);
    // Asked again, it is open already: nothing more is reported.
    f.triggers.openAtOnce(std::array<s32, 1>{104}, f.animator, f.scene, nullptr);
    CHECK(f.triggers.takeOpenings().empty());
}

TEST_CASE("closing and opening switches can reuse the same lift", "[game][world][triggers]") {
    SwitchFixture f(R"(
      {"info":0,"position":[0,0,0],"params":[0,0,2,0,2,255,0,0,0,0,0,0]},
      {"info":0,"position":[20,0,0],"params":[0,0,1,0,2,255,0,0,236,255,0,0]})");
    CHECK(f.height() == Approx(8)); // endpoint from the second registration
    std::array party{TriggerVisitor{.position = Vec3{0}}};
    f.step(0.5f, party);
    CHECK(f.triggers.opened(0));
    CHECK(f.height() == Approx(10));
    party[0].position.x = 20;
    f.step(0.5f, party);
    CHECK_FALSE(f.triggers.opened(0));
    CHECK(f.height() == Approx(8));
    party[0].position.x = 0;
    f.step(0.5f, party);
    CHECK(f.triggers.opened(0));
    CHECK(f.height() == Approx(10));
}

TEST_CASE("toggle lifts require everyone on the target and repeat after their delay",
          "[game][world][triggers]") {
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,4,5,2,255,0,0,236,255,0,0]})");
    std::array party{TriggerVisitor{.position = Vec3{0}, .floorObject = 0, .party = 0},
                     TriggerVisitor{.position = Vec3{0}, .floorObject = -1, .party = 1}};
    f.step(0.5f, party);
    CHECK_FALSE(f.triggers.opened(0));
    // The first onto it, alone, is told everyone must stand on the platform; only as it
    // comes to be occupied.
    const auto lessons = f.triggers.takeLessons();
    REQUIRE(lessons.size() == 1);
    CHECK(lessons[0].party == 0);
    CHECK(lessons[0].platform);
    f.step(0.5f, party);
    CHECK(f.triggers.takeLessons().empty());
    party[1].floorObject = 0;
    f.step(0.5f, party);
    CHECK(f.triggers.opened(0));
    CHECK(f.height() == Approx(10));
    f.step(0.5f, party);
    CHECK(f.triggers.opened(0));
    f.step(1.5f, party);
    CHECK_FALSE(f.triggers.opened(0));
    CHECK(f.height() == Approx(8));
}

TEST_CASE("a whole-party lift releases its floor boundary at the endpoint",
          "[game][world][triggers][floor-riding]") {
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,12,5,2,255,0,0,236,255,0,0]})");
    WorldCollision collision;
    std::array party{TriggerVisitor{.floorObject = 0, .party = 0},
                     TriggerVisitor{.floorObject = -1, .party = 1}};
    f.triggers.update(0.1f, party, f.animator, f.scene, &collision);
    CHECK_FALSE(collision.floorExitBlocked(0));
    party[1].floorObject = 0;
    f.triggers.update(0.1f, party, f.animator, f.scene, &collision);
    REQUIRE_FALSE(f.triggers.settled(0));
    REQUIRE(collision.floorExitBlocked(0));
    f.triggers.update(0.4f, party, f.animator, f.scene, &collision);
    REQUIRE(f.triggers.settled(0));
    CHECK_FALSE(collision.floorExitBlocked(0));
    CHECK(f.height() == Approx(10));
    // Later travel locks again; an immediate forced endpoint must also release it.
    f.triggers.update(2.0f, {}, f.animator, f.scene, &collision);
    f.triggers.update(0.1f, party, f.animator, f.scene, &collision);
    REQUIRE(collision.floorExitBlocked(0));
    f.triggers.activate(0, true, f.animator, f.scene, &collision);
    CHECK_FALSE(collision.floorExitBlocked(0));
}

TEST_CASE("animated lifts retain the party only while everyone is aboard and travel is active",
          "[game][world][triggers][floor-riding]") {
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,10,5,2,255,0,0,0,0,0,0]})",
                    R"({"name":"WALL","position":[0,10,0],"flags":4096})", 24,
                    R"({"object":0,"frames":4,"state":257,"start":0,
                        "track":{"flags":2,"frames":[0,3],"values":[0,1]}})");
    WorldCollision collision;
    std::array party{TriggerVisitor{.floorObject = 0, .party = 0},
                     TriggerVisitor{.floorObject = -1, .party = 1}};
    f.triggers.update(kStep, party, f.animator, f.scene, &collision);
    CHECK_FALSE(collision.floorExitBlocked(0));
    party[1].floorObject = 0;
    f.triggers.update(kStep, party, f.animator, f.scene, &collision);
    REQUIRE(collision.floorExitBlocked(0));
    REQUIRE_FALSE(f.triggers.settled(0));
    // Losing a rider clears whole-party readiness, even before the track ends.
    party[1].floorObject = -1;
    f.triggers.update(kStep, party, f.animator, f.scene, &collision);
    CHECK_FALSE(collision.floorExitBlocked(0));
    party[1].floorObject = 0;
    f.triggers.update(kStep, party, f.animator, f.scene, &collision);
    REQUIRE(collision.floorExitBlocked(0));
    f.animator.step(1.0f, f.scene);
    f.triggers.update(kStep, party, f.animator, f.scene, &collision);
    REQUIRE(f.triggers.settled(0));
    CHECK_FALSE(collision.floorExitBlocked(0));
}

TEST_CASE("pressure targets return when vacated", "[game][world][triggers]") {
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,0,0,2,255,0,0,0,0,20,0]})");
    const std::array party{TriggerVisitor{.position = Vec3{0}}};
    f.step(0.5f, party);
    CHECK(f.height() == Approx(12));
    f.step(0.5f);
    CHECK(f.height() == Approx(10));
    CHECK_FALSE(f.triggers.opened(0));
}

TEST_CASE("gargoyle gates accept a completed collection without opening an empty party",
          "[game][world][triggers]") {
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,66,0,2,255,101,0,0,0,20,0]})");
    CHECK_FALSE(f.triggers.opened(0));
    std::array party{TriggerVisitor{.position = Vec3{0}}};
    f.step(0.5f, party);
    CHECK_FALSE(f.triggers.opened(0));
    party[0].gargoylePieces[0] = Relics::kGargoyleNeeded[0];
    f.step(0.5f, party);
    CHECK(f.triggers.opened(0));
}

TEST_CASE("oscillating lift follows both endpoints while its pad is held",
          "[game][world][triggers]") {
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,32,0,2,255,0,0,0,0,20,0]})");
    const std::array party{TriggerVisitor{.position = Vec3{0}}};
    f.step(0.5f, party);
    CHECK(f.height() == Approx(12));
    f.step(0.5f, party);
    CHECK(f.height() == Approx(10));
    f.step(0.5f, party);
    CHECK(f.height() == Approx(12));
    f.step(0.5f);
    CHECK(f.height() == Approx(10));
}

TEST_CASE("one idle pressure pad does not cancel another on the same target",
          "[game][world][triggers]") {
    SwitchFixture f(R"(
      {"info":0,"position":[0,0,0],"params":[0,0,0,0,2,3,0,0,0,0,20,0]},
      {"info":0,"position":[20,0,0],"params":[0,0,0,0,2,4,0,0,0,0,20,0]})");
    const std::array party{TriggerVisitor{.position = Vec3{0}}};
    f.step(0.5f, party);
    CHECK(f.triggers.opened(0));
    CHECK(f.height() == Approx(12));
    const auto settled = f.triggers.takeSettled();
    REQUIRE(settled.size() == 1);
    CHECK(settled.front().sound == 3);
    CHECK(settled.front().spot == Vec3{0});
    f.step(0.5f);
    CHECK_FALSE(f.triggers.opened(0));
}

TEST_CASE("fading a target hides and unblocks its entire subtree", "[game][world][triggers]") {
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,50,0,2,255,0,0,0,0,0,0]})",
                    R"({"name":"WALL","position":[0,10,0],"flags":4096,"child":1},
                       {"name":"WALL","position":[0,2,0]})");
    WorldCollision collision;
    const std::array party{TriggerVisitor{.position = Vec3{0}}};
    f.triggers.update(1, party, f.animator, f.scene, &collision);
    for (usize i = 0; i < 2; ++i) {
        CHECK(f.scene.objectAlpha(i) == 0.0f);
        CHECK_FALSE(collision.solid(static_cast<s32>(i)));
    }
}

TEST_CASE("every catalogued level retains and activates its ordinary root switches",
          "[game][world][switch-census][unpacked]") {
    const auto root = test::unpackedOrSkip("wdata/TOWN.json").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize levels = 0;
    usize switches = 0;
    usize activated = 0;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto ref = catalog.byName(name);
            REQUIRE(ref.has_value());
            const auto file = test::unpackedOrSkip(ref->directory + "/world.json");
            WorldLayout layout;
            REQUIRE(layout.load(file.parent_path()));
            ++levels;
            WorldAnimator animator;
            animator.bind(layout);
            LevelTriggers triggers;
            triggers.bind(layout, animator, nullptr);
            switches += triggers.size();
            for (usize i = 0; i < triggers.size(); ++i) {
                animator.bind(layout);
                triggers.bind(layout, animator, nullptr);
                const auto trigger = triggers.trigger(i);
                CAPTURE(name, i, trigger.flags, trigger.target);
                if (trigger.chained ||
                    (trigger.flags & (LevelTrigger::kRequirement | LevelTrigger::kCloses)) != 0) {
                    continue;
                }
                const std::array party{
                    TriggerVisitor{.position = trigger.spot, .floorObject = trigger.target}};
                WorldScene scene;
                triggers.update(kStep, party, animator, scene, nullptr);
                CHECK(triggers.trigger(i).fired);
                if (trigger.target >= 0) {
                    CHECK(triggers.opened(trigger.target));
                }
                ++activated;
            }
        }
    }
    CAPTURE(levels, switches, activated);
    CHECK(levels > 40);
    CHECK(switches > 1000);
    CHECK(activated > 100);
}

TEST_CASE("unkeyed trigger targets translate their geometry to signed height endpoints",
          "[game][world][triggers]") {
    const auto dir = test::sampleLevel("trigger-height-endpoints");
    writeTextFile(dir / "world.json", R"({
      "objects": [{"name":"WALL", "position":[0,10,0], "flags":4096}],
      "itemInfos": [{"type":5,"subtype":23,"name":"BRIDGEPAD","radius":1}],
      "itemInstances": [{"info":0,"position":[0,0,0],
        "params":[0,0,0,0,2,255,0,0,0,0,236,255]}]
    })");
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    REQUIRE(layout.load(dir));
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    WorldScene scene;
    REQUIRE(scene.build(layout, models, textures, device));
    REQUIRE(scene.moving(0));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    const std::array visitors{TriggerVisitor{.position = Vec3{0}}};
    triggers.update(0.25f, visitors, animator, scene, nullptr);
    CHECK(scene.worldTransform(0)[3].y == Approx(9));
    CHECK(triggers.takeSettled().empty());
    triggers.update(0.25f, {}, animator, scene, nullptr);
    CHECK(scene.worldTransform(0)[3].y == Approx(8));
    const auto settled = triggers.takeSettled();
    REQUIRE(settled.size() == 1);
    CHECK(settled.front().target == 0);
    triggers.update(1, {}, animator, scene, nullptr);
    CHECK(scene.worldTransform(0)[3].y == Approx(8));
    CHECK(triggers.takeSettled().empty());
}

TEST_CASE("marker-only switches stay invisible without losing trigger behavior",
          "[triggers][trigger-visibility]") {
    SwitchFixture f(
        R"({"info":0,"flags":2,"position":[0,0,0],"params":[0,0,0,0,4,255,7,0,0,0,100,0]},
                        {"info":0,"flags":3,"position":[10,0,0],"params":[255,255,0,0,4,255,8,0,0,0,0,0]},
                        {"info":0,"flags":0,"position":[20,0,0],"params":[255,255,0,0,4,255,9,0,0,0,0,0]})");
    const auto dir = test::sampleLevel("trigger-visibility-art");
    writeTextFile(dir / "animations.json", R"({"trees":[{"name":"BRIDGEPAD",
      "nodes":[{"name":"WALL","object":"WALL","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"OFF","frames":1,"rate":30}]}]})");
    ItemArchive items;
    REQUIRE(items.load(dir));
    f.triggers.bindFigures(f.device, f.layout, items);
    f.triggers.draw(f.device, Mat4{1}, {});
    REQUIRE(f.device.draws.size() == 1);
    CHECK(f.device.draws.front().vertices.front().position.x == Approx(20));
    REQUIRE(f.triggers.size() == 3);
    const std::array visitors{TriggerVisitor{.position = Vec3{0}}};
    f.step(kStep, visitors);
    CHECK(f.triggers.trigger(0).fired);
    CHECK(f.triggers.opened(0));
    f.triggers.clear(); // Figures borrow the item archive.
}

TEST_CASE("Tower and Province trigger figures respect their authored geometry exclusions",
          "[triggers][trigger-visibility][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELL1/world.json").parent_path().parent_path().parent_path();
    for (const std::string_view level : {"L1", "G1", "E1"}) {
        CAPTURE(level);
        test::unpackedOrSkip(std::format("LEVELS/LEVEL{}/world.json", level));
        test::unpackedOrSkip(std::format("ITEMS/LEVEL{}/animations.json", level.front()));
        test::FakeRenderDevice device;
        WorldLayout layout;
        REQUIRE(layout.load(root / "LEVELS" / std::format("LEVEL{}", level)));
        WorldAnimator animator;
        animator.bind(layout);
        ItemArchive items;
        REQUIRE(items.load(root / "ITEMS" / std::format("LEVEL{}", level.front())));
        LevelTriggers triggers;
        triggers.bind(layout, animator, nullptr);
        triggers.bindFigures(device, layout, items);
        triggers.draw(device, Mat4{1}, {});
        const usize actual = device.draws.size();
        device.draws.clear();
        usize hidden = 0;
        for (usize i = 0; i < triggers.size(); ++i) {
            const auto& instance =
                layout.itemInstances()[static_cast<usize>(triggers.trigger(i).instance)];
            if ((instance.flags & 2) != 0) {
                ++hidden;
                continue;
            }
            ItemFigure visible;
            if (visible.place(device, items,
                              layout.itemInfos()[static_cast<usize>(instance.info)].name, instance,
                              nullptr)) {
                visible.draw(device, Mat4{1}, {});
            }
        }
        CHECK(hidden > 0);
        CHECK(actual == device.draws.size());
        CHECK(actual > 0); // Real physical pads have not all been removed.
    }
}

TEST_CASE("Temple bridge pads are visible and activate their authored world targets",
          "[game][world][triggers][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("ITEMS/LEVELE/animations.json");
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELE1"));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    ItemArchive items;
    REQUIRE(items.load(root / "ITEMS/LEVELE"));
    triggers.bindFigures(device, layout, items);
    triggers.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    ModelSet models;
    TextureSet textures;
    REQUIRE(models.load(root / "LEVELS/LEVELE1"));
    REQUIRE(textures.load(root / "LEVELS/LEVELE1"));
    WorldScene scene;
    const std::array<TextureSet*, 1> lenders{&items.textures};
    REQUIRE(scene.build(layout, models, textures, device, {}, lenders));
    bool tested = false;
    for (usize i = 0; i < triggers.size(); ++i) {
        const auto& trigger = triggers.trigger(i);
        const auto& instance = layout.itemInstances()[static_cast<usize>(trigger.instance)];
        const auto& info = layout.itemInfos()[static_cast<usize>(instance.info)];
        if (info.subtype == 26) {
            CHECK((trigger.flags & 0xFF) == 2);
            REQUIRE(trigger.chained);
            Vec3 activatingSpot = trigger.spot;
            for (usize parent = 0; parent < triggers.size(); ++parent) {
                if (triggers.trigger(parent).next == static_cast<s32>(i)) {
                    activatingSpot = triggers.trigger(parent).spot;
                }
            }
            const std::array visitors{TriggerVisitor{.position = activatingSpot}};
            // E1ELEV99 has no keyed animation: params[10..11] raise it by five units.
            REQUIRE_FALSE(animator.trackOf(trigger.target).has_value());
            const auto target = static_cast<usize>(trigger.target);
            REQUIRE(scene.moving(target));
            const f32 startY = scene.worldTransform(target)[3].y;
            triggers.update(kStep, visitors, animator, scene, nullptr);
            CHECK(trigger.fired);
            CHECK(triggers.opened(trigger.target));
            CHECK(scene.worldTransform(target)[3].y == Approx(startY + 4.0f * kStep));
            triggers.update(2.0f, {}, animator, scene, nullptr);
            CHECK(scene.worldTransform(target)[3].y == Approx(startY + 5.0f));
            tested = true;
        } else if (info.subtype == 23 || info.subtype == 29) {
            CHECK((trigger.flags & 0xFF) == 10);
            if (info.subtype == 29) {
                REQUIRE(animator.trackOf(trigger.target).has_value());
                const std::array visitors{TriggerVisitor{.position = trigger.spot}};
                triggers.update(kStep, visitors, animator, scene, nullptr);
                CHECK(trigger.fired);
                CHECK(triggers.opened(trigger.target));
            }
        }
    }
    REQUIRE(tested);
}

TEST_CASE("Temple floor contact opens the altar gates and both switch chains",
          "[triggers][temple-gates][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("ITEMS/LEVELE/animations.json");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("E1");
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    world.startTriggers({});
    world.updateTriggers(0, {});
    // The two altar approach gates have their markers below the traversable
    // floor. Touch them at floor height, never by teleporting to the marker.
    for (const s32 instance : {110, 112, 130, 159}) {
        if (instance == 159) {
            // The preceding switch lowers E1ELEV1, which carries the final pad.
            for (usize i = 0; i < world.triggers().size(); ++i) {
                if (world.triggers().trigger(i).id == 7) {
                    const std::array visitors{
                        TriggerVisitor{.position = world.triggers().trigger(i).spot}};
                    world.updateTriggers(kStep, visitors);
                }
            }
            for (s32 frame = 0; frame < 60; ++frame) {
                world.update(kStep);
                world.updateTriggers(kStep, {});
            }
        }
        usize index = 0;
        while (index < world.triggers().size() &&
               world.triggers().trigger(index).instance != instance) {
            ++index;
        }
        REQUIRE(index < world.triggers().size());
        const auto trigger = world.triggers().trigger(index);
        CAPTURE(instance, trigger.id);
        PlayerActor actor;
        const Vec3 approach = trigger.spot + Vec3{0, 6, 0};
        const auto floor = world.collision().floorAt(approach, 0, 20);
        REQUIRE(floor);
        actor.spawn(0, {}, nullptr, Vec3{trigger.spot.x, floor->y, trigger.spot.z + 3}, 0);
        actor.settle(world.collision());
        for (s32 frame = 0; frame < 24; ++frame) {
            actor.update(MoveInput{.direction = Vec2{0, -1}, .magnitude = 1}, 0, kStep,
                         &world.collision());
            const std::array visitors{
                TriggerVisitor{.position = actor.position(), .radius = actor.radius()}};
            world.update(kStep);
            world.updateTriggers(kStep, visitors);
        }
        CAPTURE(actor.position().x, actor.position().y, actor.position().z, trigger.spot.y);
        CHECK(world.triggers().trigger(index).fired);
        CHECK(world.triggers().opened(trigger.target));
        if (trigger.next >= 0) {
            const auto& next = world.triggers().trigger(static_cast<usize>(trigger.next));
            CHECK(next.fired);
            CHECK(world.triggers().opened(next.target));
        }
    }
}

TEST_CASE("lift pads activate from the lowered deck rather than their authored marker height",
          "[triggers][platform-contact][unpacked]") {
    const auto [name, instance] = GENERATE(std::pair{"G1", 418}, std::pair{"G1", 313},
                                           std::pair{"D4", 342}, std::pair{"J3", 483});
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName(name);
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    world.startTriggers({});
    world.updateTriggers(0, {});
    usize index = 0;
    while (index < world.triggers().size() &&
           world.triggers().trigger(index).instance != instance) {
        ++index;
    }
    REQUIRE(index < world.triggers().size());
    const auto trigger = world.triggers().trigger(index);
    PlayerActor actor;
    const auto surface = world.collision().floorAt(trigger.spot + Vec3{0, 3, 0}, 0, 30);
    REQUIRE(surface);
    actor.spawn(0, {}, nullptr, Vec3{trigger.spot.x, surface->y, trigger.spot.z}, 0);
    actor.settle(world.collision());
    const auto floor = world.collision().floorAt(actor.position(), 0.5f, 1.0f);
    REQUIRE(floor);
    CAPTURE(name, instance, trigger.target, trigger.floor, trigger.spot.y, actor.position().y,
            floor->object);
    const std::array visitors{TriggerVisitor{.position = actor.position(),
                                             .radius = actor.radius(),
                                             .floorObject = floor->object,
                                             .height = actor.height()}};
    world.updateTriggers(kStep, visitors);
    CHECK(world.triggers().trigger(index).fired);
    CHECK(world.triggers().opened(trigger.target));
}

TEST_CASE("catalogued platform pads respond at their actual supporting surface",
          "[platform-census][unpacked]") {
    const auto root = test::unpackedOrSkip("wdata/TOWN.json").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize tested = 0;
    std::set<s32> subtypes;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto ref = catalog.byName(name);
            REQUIRE(ref);
            test::unpackedOrSkip(ref->directory + "/world.json");
            WorldLayout layout;
            REQUIRE(layout.load(root / ref->directory));
            WorldAnimator preview;
            preview.bind(layout);
            LevelTriggers inventory;
            inventory.bind(layout, preview, nullptr);
            bool hasPlatforms = false;
            for (usize i = 0; i < inventory.size(); ++i) {
                hasPlatforms |= (inventory.trigger(i).flags & LevelTrigger::kOnTarget) != 0;
            }
            if (!hasPlatforms) {
                continue;
            }
            test::FakeRenderDevice device;
            LevelWorld world;
            REQUIRE(world.load(device, root, *ref));
            world.startTriggers({});
            for (usize i = 0; i < world.triggers().size(); ++i) {
                const auto& candidate = world.triggers().trigger(i);
                if (candidate.chained || candidate.target < 0 ||
                    (candidate.flags & LevelTrigger::kOnTarget) == 0 ||
                    (candidate.flags & (LevelTrigger::kCloses | LevelTrigger::kRequirement)) != 0) {
                    continue;
                }
                WorldCollision collision = world.collision();
                // Bind against world floors only, just as LevelWorld does before
                // adding collision for destructible items.
                for (usize item = 0; item < layout.itemInstances().size(); ++item) {
                    collision.setSolid(static_cast<s32>(layout.objects().size() + item), false);
                }
                WorldAnimator animator;
                animator.bind(layout);
                LevelTriggers triggers;
                triggers.bind(layout, animator, &collision);
                const auto trigger = triggers.trigger(i);
                const auto floor = collision.floorAt(trigger.spot, 4, 30);
                CAPTURE(name, trigger.instance, trigger.target, trigger.floor, trigger.spot.y);
                CHECK(floor);
                if (!floor) {
                    continue;
                }
                const s32 parent = layout.objects()[static_cast<usize>(floor->object)].parent;
                CAPTURE(floor->object, parent, floor->y);
                CHECK((floor->object == trigger.target || parent == trigger.target));
                const std::array visitors{
                    TriggerVisitor{.position = Vec3{trigger.spot.x, floor->y, trigger.spot.z},
                                   .floorObject = floor->object}};
                WorldScene scene;
                triggers.update(kStep, visitors, animator, scene, &collision);
                CHECK(triggers.trigger(i).fired);
                const auto& instance = layout.itemInstances()[static_cast<usize>(trigger.instance)];
                subtypes.insert(layout.itemInfos()[static_cast<usize>(instance.info)].subtype);
                ++tested;
            }
        }
    }
    CAPTURE(tested, subtypes.size());
    CHECK(tested > 50);
    CHECK(subtypes.size() >= 3);
}

TEST_CASE("Underworld switch artwork remains visible before and after trigger contact",
          "[triggers][underworld-switches][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELF1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("ITEMS/LEVELF/animations.json");
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELF1"));
    ItemArchive items;
    REQUIRE(items.load(root / "ITEMS/LEVELF"));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    triggers.bindFigures(device, layout, items);
    REQUIRE(triggers.size() == 11);
    WorldScene scene;
    for (usize i = 0; i < triggers.size(); ++i) {
        const auto& trigger = triggers.trigger(i);
        CAPTURE(i);
        const auto& instance = layout.itemInstances()[static_cast<usize>(trigger.instance)];
        const auto& info = layout.itemInfos()[static_cast<usize>(instance.info)];
        ItemFigure figure;
        REQUIRE(figure.place(device, items, info.name, instance, nullptr));
        device.draws.clear();
        figure.draw(device, Mat4{1}, {});
        REQUIRE_FALSE(device.draws.empty());
        const std::array visitors{TriggerVisitor{.position = trigger.spot}};
        triggers.update(kStep, visitors, animator, scene, nullptr);
        CHECK(trigger.fired);
    }
    device.draws.clear();
    triggers.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() >= triggers.size());
    triggers.clear();
}

TEST_CASE("a target on the wall is set off by what hits it, and walking past does nothing",
          "[game][world][triggers][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELC3/world.json").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELC3"));
    ModelSet models;
    TextureSet textures;
    REQUIRE(models.load(root / "LEVELS/LEVELC3"));
    REQUIRE(textures.load(root / "LEVELS/LEVELC3"));
    WorldScene scene;
    REQUIRE(scene.build(layout, models, textures, device));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    std::vector<usize> shootable;
    for (usize i = 0; i < triggers.size(); ++i) {
        if (triggers.trigger(i).shootable) {
            shootable.push_back(i);
            CHECK(triggers.trigger(i).height == 2.0f);
        }
    }
    REQUIRE(shootable.size() == 2);
    const usize first = shootable.front();
    const LevelTrigger& target = triggers.trigger(first);
    REQUIRE(target.target >= 0);
    // Someone far off: nothing. Shot: it goes off on the next update, once.
    const std::array away{TriggerVisitor{.position = target.spot + Vec3{200, 0, 0}}};
    triggers.update(kStep, away, animator, scene, nullptr);
    REQUIRE_FALSE(target.fired);
    triggers.shoot(first);
    triggers.update(kStep, away, animator, scene, nullptr);
    CHECK(target.fired);
    CHECK(triggers.opened(target.target));
    CHECK_FALSE(target.shot);
    // Only a shootable trigger takes a shot.
    for (usize i = 0; i < triggers.size(); ++i) {
        if (!triggers.trigger(i).shootable) {
            triggers.shoot(i);
            CHECK_FALSE(triggers.trigger(i).shot);
            break;
        }
    }
}

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    WorldAnimator animator;
    WorldCollision collision;
    LevelTriggers triggers;

    explicit Fixture(std::string_view name) {
        const auto dir = test::sampleLevel(name);
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(layout.load(dir));
        REQUIRE(scene.build(layout, models, textures, device));
        animator.bind(layout);
        // A wall of the torch's own, so its blocking can be watched.
        CollisionTriangle wall;
        wall.object = 6;
        wall.normal = Vec3{0.0f, 0.0f, -1.0f};
        wall.vertices = {Vec3{0.0f, 0.0f, 30.0f}, Vec3{20.0f, 0.0f, 30.0f},
                         Vec3{20.0f, 5.0f, 30.0f}};
        collision.build({wall});
        triggers.bind(layout, animator, &collision);
    }

    static TriggerVisitor visitor(const Vec3& position, s32 crystals) {
        TriggerVisitor out;
        out.position = position;
        out.crystals[1] = crystals;
        return out;
    }
};

TEST_CASE("triggers name their targets, chain, and hold animated ones at their start",
          "[game][world][triggers]") {
    Fixture f("level-triggers");
    REQUIRE(f.triggers.size() == 3);
    const LevelTrigger& field = f.triggers.trigger(0);
    REQUIRE(field.target == 6);
    REQUIRE(field.id == 1);
    REQUIRE(field.needsCrystals());
    REQUIRE(field.radius == 2.0f);
    REQUIRE((field.kind & LevelTrigger::kFades) != 0);
    REQUIRE(field.next == -1);
    const LevelTrigger& gate = f.triggers.trigger(1);
    REQUIRE(gate.target == 8);
    REQUIRE_FALSE(gate.needsCrystals());
    REQUIRE(gate.nextId == 6);
    REQUIRE(gate.next == 2); // chained to the far pane's trigger
    REQUIRE_FALSE(gate.chained);
    REQUIRE(f.triggers.trigger(2).target == 9);
    REQUIRE(f.triggers.trigger(2).chained);
    REQUIRE(LevelTriggers::crystalsNeeded(1) == 15);
    REQUIRE(LevelTriggers::crystalsNeeded(0) == 0);
    REQUIRE(LevelTriggers::crystalsNeeded(40) == 0);
    // The spinning group waits at its first frame instead of looping.
    REQUIRE(f.animator.held(0));
    f.animator.step(kStep, f.scene);
    f.animator.step(kStep, f.scene);
    REQUIRE(f.animator.frame(0) == 0.0f);
}

TEST_CASE("a visitor sets off a trigger, opening its chain, once", "[game][world][triggers]") {
    Fixture f("level-triggers-gate");
    std::vector<TriggerVisitor> party{Fixture::visitor(Vec3{10.0f, 0.0f, 55.0f}, 0)};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE_FALSE(f.triggers.trigger(1).fired); // still out of reach
    // Standing on the far pane's own trigger does nothing: chained after the gate's, it is
    // set off through that alone.
    party[0].position = Vec3{10.0f, 0.0f, 100.0f};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE_FALSE(f.triggers.trigger(2).fired);
    REQUIRE_FALSE(f.triggers.opened(9));
    party[0].position = Vec3{10.0f, 0.0f, 52.0f};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.trigger(1).fired);
    REQUIRE(f.triggers.trigger(2).fired); // the chain
    const auto cameraCues = f.triggers.takeCameraCues();
    REQUIRE(cameraCues.size() == 2);
    CHECK(cameraCues[0].id == f.triggers.trigger(1).id);
    CHECK(cameraCues[1].id == f.triggers.trigger(2).id);
    REQUIRE(f.triggers.opened(8));
    REQUIRE_FALSE(f.animator.held(0));
    // The gate plays its turn once and stays open.
    for (s32 i = 0; i < 10; ++i) {
        f.animator.step(kStep, f.scene);
    }
    REQUIRE(f.animator.frame(0) == 3.0f);
    REQUIRE(f.animator.finished(0));
    // Run to its end, the animated target is reported settled once; the plain one never is.
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    const std::vector<TriggerOpening> settled = f.triggers.takeSettled();
    REQUIRE(settled.size() == 1);
    REQUIRE(settled[0].target == 8);
    REQUIRE_FALSE(settled[0].fades);
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.takeSettled().empty());
}

TEST_CASE("a trigger the party starts inside accepts contact on the first update",
          "[game][world][triggers]") {
    Fixture f("level-triggers-gate");
    std::vector<TriggerVisitor> party{Fixture::visitor(Vec3{10.0f, 0.0f, 52.0f}, 0)};
    f.triggers.openMet(party, f.animator, f.scene, &f.collision);
    for (s32 i = 0; i < 10; ++i) {
        f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    }
    REQUIRE(f.triggers.trigger(1).fired);
    REQUIRE(f.triggers.takeOpenings().size() == 2);
    // Re-entering a latched pad does not restart its opening.
    party[0].position = Vec3{10.0f, 0.0f, 70.0f};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    party[0].position = Vec3{10.0f, 0.0f, 52.0f};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.trigger(1).fired);
    REQUIRE(f.triggers.takeOpenings().empty());
}

TEST_CASE("a field wants the realm's crystals, then fades and stops blocking",
          "[game][world][triggers]") {
    Fixture f("level-triggers-field");
    REQUIRE(f.collision.solid(6));
    // Short of crystals, the spot reaches its own radius only: nothing from 3.5 units off.
    std::vector<TriggerVisitor> party{Fixture::visitor(Vec3{10.0f, 0.0f, 26.5f}, 3)};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.takeRefusals().empty());
    party[0].position = Vec3{10.0f, 0.0f, 29.0f};
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE_FALSE(f.triggers.trigger(0).fired);
    REQUIRE(f.collision.solid(6));
    REQUIRE(f.scene.objectAlpha(6) == 1.0f);
    // Standing there short of crystals is refused, once, then again after the cooldown.
    std::vector<TriggerRefusal> refusals = f.triggers.takeRefusals();
    REQUIRE(refusals.size() == 1);
    REQUIRE(refusals[0].trigger == 0);
    REQUIRE(refusals[0].id == 1);
    REQUIRE(refusals[0].crystals);
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.takeRefusals().empty());
    f.triggers.update(LevelTriggers::kRefusalCooldown, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.takeRefusals().size() == 1);
    REQUIRE(f.triggers.takeOpenings().empty());
    // One member carrying enough opens it for everyone (towerAllPlayersMetBossReq), and the
    // spot then reaches twice as far.
    party[0].position = Vec3{10.0f, 0.0f, 26.5f};
    party.push_back(Fixture::visitor(Vec3{50.0f, 0.0f, 50.0f}, 15));
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.trigger(0).fired);
    REQUIRE_FALSE(f.collision.solid(6));
    // Its opening is reported once: the fading field before the party.
    std::vector<TriggerOpening> openings = f.triggers.takeOpenings();
    REQUIRE(openings.size() == 1);
    REQUIRE(openings[0].target == 6);
    REQUIRE(openings[0].fades);
    REQUIRE_FALSE(openings[0].atOnce);
    REQUIRE(openings[0].spot == f.triggers.trigger(0).spot);
    REQUIRE(f.triggers.takeOpenings().empty());
    REQUIRE(f.triggers.takeRefusals().empty());
    REQUIRE(f.scene.objectAlpha(6) == Approx(1.0f - LevelTriggers::kFadeRate));
    REQUIRE(f.triggers.takeSettled().empty()); // still thinning
    for (s32 i = 0; i < 20; ++i) {
        f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    }
    REQUIRE(f.scene.objectAlpha(6) == 0.0f);
    REQUIRE(f.triggers.alphaOf(6) == 0.0f);
    // Gone, the field is reported settled once, with its trigger's sound slot.
    const std::vector<TriggerOpening> settled = f.triggers.takeSettled();
    REQUIRE(settled.size() == 1);
    REQUIRE(settled[0].target == 6);
    REQUIRE(settled[0].fades);
    REQUIRE(settled[0].sound == 0);
    REQUIRE(f.triggers.takeSettled().empty());
    f.device.draws.clear();
    f.scene.draw(f.device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 0.0f});
    for (const auto& draw : f.device.draws) {
        REQUIRE(draw.vertices[0].position != Vec3{10.0f, 0.0f, 30.0f}); // the torch is gone
    }
}

TEST_CASE("one member meets a crystal gate by count, a completed record or being Sumner",
          "[game][world][triggers]") {
    TriggerVisitor visitor;
    visitor.crystals[1] = 14;
    CHECK_FALSE(LevelTriggers::crystalsMet(visitor, 1));
    visitor.crystals[1] = 15;
    CHECK(LevelTriggers::crystalsMet(visitor, 1));
    visitor.crystals[1] = -1;
    CHECK(LevelTriggers::crystalsMet(visitor, 1));
    visitor.crystals[1] = 0;
    visitor.sumner = true;
    CHECK(LevelTriggers::crystalsMet(visitor, 1));
    CHECK_FALSE(LevelTriggers::crystalsMet(visitor, -1));
    CHECK_FALSE(LevelTriggers::crystalsMet(visitor, static_cast<s32>(kRealmCount)));
}

TEST_CASE("what the party already qualifies for opens at once when the level starts",
          "[game][world][triggers]") {
    Fixture f("level-triggers-start");
    const std::vector<TriggerVisitor> party{Fixture::visitor(Vec3{0.0f, 0.0f, 0.0f}, 15)};
    f.triggers.openMet(party, f.animator, f.scene, &f.collision);
    REQUIRE(f.triggers.trigger(0).fired);
    CHECK(f.triggers.takeCameraCues().empty());
    REQUIRE(f.triggers.alphaOf(6) == 0.0f);
    REQUIRE_FALSE(f.collision.solid(6));
    REQUIRE_FALSE(f.triggers.trigger(1).fired); // gates want a visitor
    f.triggers.clear();
    REQUIRE(f.triggers.size() == 0);
}

} // namespace
