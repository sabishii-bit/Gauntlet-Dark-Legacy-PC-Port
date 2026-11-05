#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <set>
#include <string>
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
#include "engine/render/ImmediateBatch.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/critters/CritterStatues.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelTriggers.h"
#include "game/level/LevelWorld.h"
#include "game/party/FloorRiding.h"
#include "game/players/PlayerActor.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

TEST_CASE("G3 fountain completion lowers its fire emitters with the native fence chain",
          "[triggers][particles][g3-fountain][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(catalog.byName("G3").has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G3")));
    world.startTriggers({});
    const auto& objects = world.layout().objects();
    std::vector<usize> fences;
    for (const std::string_view name : {"G3FENCE22", "G3FENCE23", "G3FENCE24", "G3FENCE25#0"}) {
        for (usize i = 0; i < objects.size(); ++i) {
            if (objects[i].name == name) {
                fences.push_back(i);
            }
        }
    }
    REQUIRE(fences.size() == 4);
    std::vector<std::pair<usize, usize>> flames;
    for (usize object = 0; object < objects.size(); ++object) {
        if (!objects[object].particles()) {
            continue;
        }
        bool attached = false;
        for (s32 parent = objects[object].parent; parent >= 0;
             parent = objects[static_cast<usize>(parent)].parent) {
            for (const usize fence : fences) {
                attached |= static_cast<usize>(parent) == fence;
            }
        }
        if (!attached) {
            continue;
        }
        REQUIRE((objects[object].flags & WorldObject::kAnimated) != 0);
        const Vec3 position = world.layout().worldPosition(object);
        usize found = world.particles().size();
        for (usize emitter = 0; emitter < world.particles().size(); ++emitter) {
            if (glm::distance(Vec3{world.particles().emitter(emitter).node()[3]}, position) <
                0.01f) {
                REQUIRE(found == world.particles().size());
                found = emitter;
            }
        }
        REQUIRE(found < world.particles().size());
        flames.emplace_back(object, found);
    }
    REQUIRE(flames.size() == 20);
    usize completion = world.triggers().size();
    for (usize i = 0; i < world.triggers().size(); ++i) {
        if (world.triggers().trigger(i).instance == 396) {
            completion = i;
        }
    }
    REQUIRE(completion < world.triggers().size());
    const auto& trigger = world.triggers().trigger(completion);
    REQUIRE(trigger.id == 39);
    REQUIRE_FALSE(trigger.chained);
    // No puzzle completion: the held native tracks leave the fire at walking height.
    for (s32 frame = 0; frame < 90; ++frame) {
        world.update(kStep);
        world.updateTriggers(kStep, {});
    }
    REQUIRE_FALSE(trigger.fired);
    for (const auto& [object, emitter] : flames) {
        REQUIRE_FALSE(world.particles().emitter(emitter).particles().empty());
        CHECK(Vec3{world.particles().emitter(emitter).node()[3]}.y ==
              Approx(world.layout().worldPosition(object).y));
    }
    // Enter the authored completion trigger, not a forced emitter shutdown. Its chain
    // plays all four 62-frame fence tracks; their final Y offset is -20.029818 units.
    const std::array visitors{
        TriggerVisitor{.position = trigger.spot, .floorObject = trigger.floor}};
    world.updateTriggers(kStep, visitors);
    REQUIRE(trigger.fired);
    for (s32 frame = 0; frame < 240; ++frame) {
        world.update(kStep);
        world.updateTriggers(kStep, {});
    }
    for (const usize fence : fences) {
        CHECK(world.triggers().opened(static_cast<s32>(fence)));
        CHECK(Vec3{world.scene().worldTransform(fence)[3]}.y ==
              Approx(world.layout().worldPosition(fence).y - 20.029818f).margin(0.001f));
    }
    for (const auto& [object, index] : flames) {
        CAPTURE(object, index);
        const auto& emitter = world.particles().emitter(index);
        const Vec3 liveNode{world.scene().worldTransform(object)[3]};
        const Vec3 originalNode = world.layout().worldPosition(object);
        CHECK(glm::distance(Vec3{emitter.node()[3]}, liveNode) < 0.001f);
        // Native does not delete these emitters: new fire is below the floor, while
        // particles born before the fences descended have lived out their short lives.
        CHECK(emitter.active());
        REQUIRE_FALSE(emitter.particles().empty());
        for (const auto& particle : emitter.particles()) {
            CHECK(particle.origin.y < world.layout().worldPosition(object).y - 10.0f);
        }
        // Full quads must clear the walking floor at each original marker, including
        // rising motion and expanding widths. Billboard corners may extend off its edge.
        const auto floor = world.collision().floorAt(originalNode, 4, 10);
        REQUIRE(floor.has_value());
        CHECK(emitter.descriptor().depthTest);
        for (const f32 frameOffset : {-1.0f, 0.0f, 1.0f}) {
            ImmediateBatch quads;
            quads.begin(PrimitiveTopology::TriangleList);
            emitter.draw(quads, Vec3{1, 0, 0}, Vec3{0, 1, 0}, frameOffset);
            quads.end();
            REQUIRE_FALSE(quads.empty());
            f32 highest = liveNode.y;
            for (const auto& vertex : quads.triangles()) {
                highest = std::max(highest, vertex.position.y);
            }
            CHECK(highest < floor->y);
        }
    }
}

TEST_CASE("G1 upper landing calls the lift before walking down to the bridge logs and key",
          "[triggers][g1-bridge-logs][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G1")));
    world.startTriggers({});
    world.updateTriggers(0, {});
    const auto indexOf = [&](s32 instance) {
        for (usize i = 0; i < world.triggers().size(); ++i) {
            if (world.triggers().trigger(i).instance == instance) {
                return i;
            }
        }
        return world.triggers().size();
    };
    const usize call = indexOf(308);
    const usize aboard = indexOf(313);
    const usize logA = indexOf(53);
    const usize logB = indexOf(54);
    REQUIRE(call < world.triggers().size());
    REQUIRE(aboard < world.triggers().size());
    REQUIRE(logA < world.triggers().size());
    REQUIRE(logB < world.triggers().size());
    CHECK(world.triggers().trigger(call).floor == -1);
    CHECK(world.triggers().trigger(call).spot.y == Approx(27.3701f).margin(0.02f));
    CHECK(world.triggers().trigger(aboard).floor == 227);
    CHECK(world.triggers().trigger(aboard).spot.y == Approx(19.3891f).margin(0.02f));
    std::array<PlayerRuntime, 1> party;
    auto& actor = party[0].actor;
    actor.spawn(0, {}, nullptr, {53, 27.3f, -209.84f}, 0);
    actor.settle(world.collision());
    FloorRiding::land(party, 0, actor.position(), world.collision());
    // The bridge's breakable parapet guards the lift; its collision must be broken
    // through the same damage path before the character can reach the landing.
    bool brokeParapet = false;
    for (usize i = 0; i < world.walls().size(); ++i) {
        const auto& wall = world.walls().wall(i);
        if (glm::distance(Vec3{wall.transform[3]}, Vec3{48.4765625f, 28.7109375f, -208.734375f}) <
            0.1f) {
            REQUIRE(world.strikeWall(i, 1000) == 0);
            brokeParapet = true;
        }
    }
    REQUIRE(brokeParapet);
    const auto step = [&](Vec2 direction = Vec2{0}) {
        world.update(kStep);
        FloorRiding::carry(party[0], world.collision());
        const Vec3 before = actor.position();
        actor.update(MoveInput{.direction = direction,
                               .magnitude = glm::length(direction) > 0 ? 1.0f : 0.0f},
                     0, kStep, &world.collision());
        actor.fall(kStep, world.collision());
        FloorRiding::land(party, 0, before, world.collision());
        const std::array visitors{TriggerVisitor{.position = actor.position(),
                                                 .radius = actor.reach(), // as PlayScene has it
                                                 .floorObject = party[0].floor.object,
                                                 .height = actor.height()}};
        world.updateTriggers(kStep, visitors);
    };
    const auto walk = [&](Vec2 goal) {
        for (s32 frame = 0; frame < 180; ++frame) {
            const Vec2 delta = goal - Vec2{actor.position().x, actor.position().z};
            if (glm::length(delta) < 0.6f) {
                return;
            }
            step(glm::normalize(delta));
        }
        CAPTURE(goal.x, goal.y, actor.position().x, actor.position().y, actor.position().z);
        FAIL("walking route did not reach its waypoint");
    };
    // The body stops a width from the parapet's neighbours; the switch reaches it there.
    walk({50.3f, -209.84f});
    for (s32 frame = 0; frame < 30 && !world.triggers().trigger(call).fired; ++frame) {
        step({-1, 0});
    }
    CAPTURE(actor.position().x, actor.position().y, actor.position().z, actor.radius());
    REQUIRE(world.triggers().trigger(call).fired);
    for (s32 frame = 0; frame < 120; ++frame) {
        step();
    }
    walk({46.875f, -208.28f});
    for (s32 frame = 0; frame < 180 && actor.position().y > 19.4f; ++frame) {
        step();
    }
    CAPTURE(actor.position().x, actor.position().y, actor.position().z);
    REQUIRE(world.triggers().trigger(aboard).fired);
    CHECK(actor.position().y == Approx(19.2891f).margin(0.2f));
    walk({48.9f, -204});
    walk({54.6875f, -203.0f});
    REQUIRE(world.triggers().trigger(logA).fired);
    REQUIRE(world.triggers().trigger(logB).fired);
    for (s32 frame = 0; frame < 180; ++frame) {
        step();
    }
    walk({58.96f, -205.89f});
    // This bonus key is authored for two or more players, not a missing solo key.
    usize key = world.placedItems().size();
    for (usize i = 0; i < world.placedItems().size(); ++i) {
        if (world.placedItems().item(i).instance == 307) {
            key = i;
            break;
        }
    }
    REQUIRE(key < world.placedItems().size());
    CHECK_FALSE(world.placedItems().item(key).visible);
    world.setPlayerCount(2);
    CHECK(world.placedItems().item(key).visible);
    const std::array collectors{Collector{actor.position(), actor.radius(), actor.height()}};
    const auto pickups = world.collect(device, collectors);
    bool tookKey = false;
    for (const auto& pickup : pickups) {
        tookKey |= world.placedItems().item(pickup.item).instance == 307;
    }
    CHECK(tookKey);
}

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

TEST_CASE("trigger eligibility uses the joined party and exact-count variants",
          "[triggers][trigger-party-gates]") {
    const auto minimum = GENERATE(0, 1, 2, 3, 4, 11, 12, 13, 14);
    const auto joined = GENERATE(1, 2, 3, 4);
    SwitchFixture f(std::format(
        R"({{"info":0,"minPlayers":{},"position":[0,0,0],"params":[0,0,0,0,2,255,0,0,0,0,0,0]}})",
        minimum));
    f.triggers.setPlayerCount(joined);
    // Fallen members still count for the placement variant, but do not stand on it.
    const std::array standing{TriggerVisitor{.party = 3}};
    f.step(kStep, standing);
    const bool eligible = minimum > 10 ? joined == minimum - 10 : joined >= minimum;
    CAPTURE(minimum, joined);
    CHECK(f.triggers.trigger(0).enabled == eligible);
    CHECK(f.triggers.trigger(0).fired == eligible);
    CHECK(f.triggers.opened(0) == eligible);
}

TEST_CASE(
    "a disabled trigger cannot initiate a chain but an eligible successor survives a hidden link",
    "[triggers][trigger-party-gates][trigger-chain-party]") {
    const auto rootMinimum = GENERATE(1, 2);
    SwitchFixture f(
        std::format(
            R"({{"info":0,"minPlayers":{},"position":[0,0,0],"params":[255,255,2,0,2,255,1,2,0,0,0,0]}},
           {{"info":0,"minPlayers":2,"position":[100,0,0],"params":[0,0,2,0,2,255,2,3,0,0,0,0]}},
           {{"info":0,"minPlayers":1,"position":[200,0,0],"params":[1,0,2,0,2,255,3,0,0,0,0,0]}})",
            rootMinimum),
        R"({"name":"WALL","position":[0,10,0]}, {"name":"WALL","position":[0,20,0]})");
    f.triggers.setPlayerCount(1);
    const std::array standing{TriggerVisitor{.party = 0}};
    f.step(kStep, standing);
    CHECK_FALSE(f.triggers.trigger(1).enabled);
    CHECK_FALSE(f.triggers.trigger(1).fired);
    CHECK_FALSE(f.triggers.opened(0));
    CHECK(f.triggers.opened(1) == (rootMinimum == 1));
    f.triggers.setPlayerCount(2);
    f.step(kStep, standing);
    CHECK(f.triggers.trigger(1).enabled);
    CHECK(f.triggers.opened(0));
    CHECK(f.triggers.opened(1));
}

TEST_CASE("whole-party successors combine player identities from distinct contact roots",
          "[triggers][trigger-chain-party]") {
    SwitchFixture f(
        R"({"info":0,"position":[0,0,0],"params":[255,255,2,0,2,255,0,9,0,0,0,0]},
           {"info":0,"position":[10,0,0],"params":[255,255,2,0,2,255,0,9,0,0,0,0]},
           {"info":0,"position":[100,0,0],"params":[0,0,2,4,2,255,9,0,0,0,0,0]})");
    f.triggers.setPlayerCount(4);
    std::array standing{TriggerVisitor{.party = 0},
                        TriggerVisitor{.position = Vec3{1000, 0, 0}, .party = 3}};
    f.step(kStep, standing);
    CHECK(f.triggers.trigger(0).fired);
    CHECK_FALSE(f.triggers.trigger(2).fired);
    CHECK_FALSE(f.triggers.opened(0));
    standing[1].position = Vec3{10, 0, 0};
    f.step(kStep, standing);
    CHECK(f.triggers.trigger(2).fired);
    CHECK(f.triggers.opened(0));
    // Joined-but-fallen slots 1 and 2 do not block a standing-party requirement.
    CHECK(f.triggers.takeOpenings().size() == 1);
}

TEST_CASE("a linked floor requirement filters only that node's contact identities",
          "[triggers][trigger-chain-party]") {
    SwitchFixture f(
        R"({"info":0,"position":[0,0,0],"params":[255,255,2,0,2,255,1,2,0,0,0,0]},
           {"info":0,"position":[100,0,0],"params":[0,0,2,1,2,255,2,3,0,0,0,0]},
           {"info":0,"position":[200,0,0],"params":[1,0,2,0,2,255,3,0,0,0,0,0]})",
        R"({"name":"WALL","position":[0,10,0]}, {"name":"WALL","position":[0,20,0]})");
    std::array standing{TriggerVisitor{.party = 2}};
    f.step(kStep, standing);
    CHECK_FALSE(f.triggers.opened(0));
    CHECK(f.triggers.opened(1));
    standing[0].floorObject = 0;
    f.step(kStep, standing);
    CHECK(f.triggers.opened(0));
}

TEST_CASE("joined party changes re-enable retained triggers without replaying disabled shots",
          "[triggers][trigger-party-gates]") {
    SwitchFixture f(
        R"({"info":0,"minPlayers":12,"position":[0,0,0],"params":[0,0,2,0,2,255,0,0,0,0,0,0]})",
        R"({"name":"WALL","position":[0,10,0]})", 31);
    const std::array standing{TriggerVisitor{.position = Vec3{100, 0, 0}, .party = 0}};
    f.triggers.setPlayerCount(1);
    f.triggers.shoot(0);
    f.step(kStep, standing);
    CHECK_FALSE(f.triggers.trigger(0).shot);
    CHECK_FALSE(f.triggers.trigger(0).enabled);
    f.triggers.setPlayerCount(2);
    f.step(kStep, standing);
    CHECK(f.triggers.trigger(0).enabled);
    CHECK_FALSE(f.triggers.opened(0));
    f.triggers.shoot(0);
    f.step(kStep, standing);
    CHECK(f.triggers.opened(0));
    f.triggers.setPlayerCount(3);
    CHECK_FALSE(f.triggers.trigger(0).enabled);
    f.triggers.setPlayerCount(2);
    CHECK(f.triggers.trigger(0).enabled);
    CHECK(f.triggers.opened(0));
}

TEST_CASE("a root's floor requirement prevents contact from reaching its successors",
          "[triggers][trigger-chain-party]") {
    SwitchFixture f(
        R"({"info":0,"position":[0,0,0],"params":[0,0,2,1,2,255,1,2,0,0,0,0]},
           {"info":0,"position":[100,0,0],"params":[1,0,2,0,2,255,2,0,0,0,0,0]})",
        R"({"name":"WALL","position":[0,10,0]}, {"name":"WALL","position":[0,20,0]})");
    std::array standing{TriggerVisitor{.party = 0}};
    f.step(kStep, standing);
    CHECK_FALSE(f.triggers.opened(0));
    CHECK_FALSE(f.triggers.opened(1));
    standing[0].floorObject = 0;
    f.step(kStep, standing);
    CHECK(f.triggers.opened(0));
    CHECK(f.triggers.opened(1));
}

TEST_CASE("an event camera preserves pressure contact only until it releases the party",
          "[triggers][trigger-chain-party][trigger-camera-hold]") {
    SwitchFixture f(
        R"({"info":0,"position":[0,0,0],"params":[0,0,0,0,2,255,1,2,0,0,0,0]},
           {"info":0,"position":[100,0,0],"params":[1,0,0,0,2,255,2,0,0,0,0,0]})",
        R"({"name":"WALL","position":[0,10,0]}, {"name":"WALL","position":[0,20,0]})");
    std::array standing{TriggerVisitor{.party = 2}};
    f.triggers.setPlayerCount(1);
    f.step(kStep, standing);
    REQUIRE(f.triggers.opened(0));
    REQUIRE(f.triggers.opened(1));
    f.triggers.setCameraHeld(true);
    standing[0].position.x = 1000;
    f.step(kStep, standing);
    CHECK(f.triggers.opened(0));
    CHECK(f.triggers.opened(1));
    SECTION("release forgets the old contacts") {
        f.triggers.setCameraHeld(false);
        f.step(kStep, standing);
    }
    SECTION("a player who leaves the standing party cannot hold a pad") {
        f.step(kStep);
    }
    CHECK_FALSE(f.triggers.opened(0));
    CHECK_FALSE(f.triggers.opened(1));
}

TEST_CASE("A5's native linked whole-party switch rejects a single participant's contact",
          "[triggers][trigger-chain-party][assets]") {
    const auto file = test::assetOrSkip("LEVELS/LEVELA5/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(file.parent_path()));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    triggers.setPlayerCount(2);
    usize root = triggers.size();
    for (usize i = 0; i < triggers.size(); ++i) {
        if (triggers.trigger(i).instance == 1) {
            root = i;
            break;
        }
    }
    REQUIRE(root < triggers.size());
    const auto& trigger = triggers.trigger(root);
    REQUIRE(trigger.next >= 0);
    const auto& successor = triggers.trigger(static_cast<usize>(trigger.next));
    REQUIRE(successor.target == 25);
    REQUIRE((successor.flags & LevelTrigger::kWholeParty) != 0);
    std::array standing{TriggerVisitor{.position = trigger.spot, .party = 0},
                        TriggerVisitor{.position = trigger.spot + Vec3{1000, 0, 0}, .party = 1}};
    WorldScene scene;
    triggers.update(kStep, standing, animator, scene, nullptr);
    CHECK(trigger.fired);
    CHECK_FALSE(successor.fired);
    CHECK_FALSE(triggers.opened(25));
    standing[1].position = trigger.spot;
    triggers.update(kStep, standing, animator, scene, nullptr);
    CHECK(successor.fired);
    CHECK(triggers.opened(25));
}

TEST_CASE(
    "trigger contact reaches a height and one below the feet, the body's height further above",
    "[triggers][temple-gates]") {
    // fn_8005F0F4 compares the body's centre, half its height up, with the item's contact
    // point, a unit above its base, within the item's height plus the body's half.
    const auto padAt = [](f32 y) {
        return std::format(
            R"({{"info":0,"position":[0,{},0],"params":[0,0,0,0,0,255,0,0,0,0,0,0]}})", y);
    };
    constexpr std::string_view kWall = R"({"name":"WALL","position":[0,10,0]})";
    SECTION("below the feet, its height and one down, whatever the body's height") {
        SwitchFixture shallow(padAt(-3.9f), kWall, 24, "", 3.0f);
        std::array party{TriggerVisitor{.position = Vec3{0}, .height = 5.0f}};
        shallow.step(kStep, party);
        CHECK(shallow.triggers.trigger(0).fired);
        SwitchFixture deep(padAt(-4.1f), kWall, 24, "", 3.0f);
        deep.step(kStep, party);
        CHECK_FALSE(deep.triggers.trigger(0).fired);
        party[0].height = 50.0f;
        deep.step(kStep, party);
        CHECK_FALSE(deep.triggers.trigger(0).fired);
    }
    SECTION("above the feet, its height and the body's less one up") {
        SwitchFixture f(padAt(6.9f), kWall, 24, "", 3.0f);
        std::array party{TriggerVisitor{.position = Vec3{0}, .height = 3.0f}};
        f.step(kStep, party);
        CHECK_FALSE(f.triggers.trigger(0).fired);
        party[0].height = 5.0f;
        f.step(kStep, party);
        CHECK(f.triggers.trigger(0).fired);
    }
    SECTION("sideways, its radius and the visitor's") {
        SwitchFixture f(padAt(0.0f), kWall, 24, "", 3.0f);
        std::array party{TriggerVisitor{.position = Vec3{3, 0, 0}}};
        f.step(kStep, party);
        CHECK_FALSE(f.triggers.trigger(0).fired);
    }
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
    f.triggers.bindFigures(f.device, f.layout, AssetScope(items));
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

TEST_CASE("a lowered lift does not take its upper landing call switch down with it",
          "[triggers][platform-contact][initial-floor-binding]") {
    SwitchFixture f(
        R"({"info":0,"position":[0.2,10.1,0],
            "params":[0,0,10,0,255,255,0,0,176,255,0,0]},
           {"info":0,"position":[-3,10.1,0],
            "params":[0,0,12,5,1,255,0,0,176,255,0,0]})",
        R"({"name":"WALL","position":[0,10.1,0],"flags":4096},
           {"name":"FLOOR","position":[0,10,0]})");
    CollisionTriangle lift;
    lift.object = 0;
    lift.objectFlags = WorldObject::kFloor | WorldObject::kAnimated;
    lift.normal = Vec3{0, 1, 0};
    lift.vertices = {Vec3{-10, 0, -10}, Vec3{0.5f, 0, -10}, Vec3{0.5f, 0, 10}};
    CollisionTriangle landing;
    landing.object = 1;
    landing.objectFlags = WorldObject::kFloor;
    landing.normal = Vec3{0, 1, 0};
    landing.vertices = {Vec3{0, 10, -10}, Vec3{10, 10, -10}, Vec3{0, 10, 10}};
    WorldCollision collision;
    collision.build({lift, landing});
    collision.setMovingObjects(std::array<s32, 1>{0});
    // At rest the lift is just higher than the landing within the floor probe.
    // Binding must use its -8 closed endpoint, not attach both markers to it.
    for (const f32 existingHeight : {10.1f, 2.1f}) {
        CAPTURE(existingHeight);
        collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, existingHeight, 0}));
        f.triggers.bind(f.layout, f.animator, &collision);
        REQUIRE(f.triggers.size() == 2);
        CHECK(f.triggers.trigger(0).floor == -1);
        CHECK(f.triggers.trigger(0).spot.y == Approx(10.1f));
        CHECK(f.triggers.trigger(1).floor == 0);
        CHECK(f.triggers.trigger(1).spot.y == Approx(2.2f));
        CHECK((f.triggers.trigger(1).flags & LevelTrigger::kOnTarget) != 0);
        // A projection for binding does not mutate the caller's collision.
        CHECK((*collision.objectTransform(0))[3].y == Approx(existingHeight));
    }
    const std::array visitor{TriggerVisitor{.position = Vec3{0.2f, 10, 0}}};
    f.triggers.update(kStep, visitor, f.animator, f.scene, &collision);
    CHECK(f.triggers.trigger(0).fired);
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

TEST_CASE("a zero-travel switch settles its opening cue once",
          "[triggers][trigger-zero-travel][ice-boiler-audio]") {
    // Ice Camp's boiler rotation uses a switch target with equal height endpoints.
    SwitchFixture f(R"({"info":0,"position":[0,0,0],
      "params":[0,0,2,16,0,0,5,0,0,0,0,0]})");
    const std::array party{TriggerVisitor{.position = Vec3{0}}};
    f.step(kStep, party);
    const auto openings = f.triggers.takeOpenings();
    REQUIRE(openings.size() == 1);
    REQUIRE(f.triggers.settled(0));
    const auto settled = f.triggers.takeSettled();
    REQUIRE(settled.size() == 1);
    CHECK(settled.front().target == openings.front().target);
    CHECK(settled.front().sound == openings.front().sound);
    CHECK(f.height() == Approx(10));
    f.step(kStep, party);
    CHECK(f.triggers.takeOpenings().empty());
    CHECK(f.triggers.takeSettled().empty());
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

TEST_CASE("a platform that goes clear as it moves holds still under a player standing on it",
          "[game][world][triggers][platform-riders]") {
    // ProcessItemWobjs skips the height step while did_generate finds a player on the
    // object, unless the target keeps blocking (0x8): the platform waits for its rider.
    // A params-driven switch (subtype 24) keeps blocking by default; subtype 26's table
    // flags carry no such bit, so its platform waits for a rider.
    const bool staysSolid = GENERATE(false, true);
    SwitchFixture f(R"({"info":0,"position":[20,0,0],
      "params":[0,0,0,0,2,255,0,0,0,0,16,255]})",
                    R"({"name":"WALL","position":[0,10,0],"flags":4096})", staysSolid ? 24 : 26);
    WorldCollision collision;
    const std::array button{TriggerVisitor{.position = Vec3{20, 0, 0}, .party = 0}};
    const std::array both{TriggerVisitor{.position = Vec3{20, 0, 0}, .party = 0},
                          TriggerVisitor{.position = Vec3{0, 10, 0}, .floorObject = 0, .party = 1}};
    f.triggers.update(1.0f / 30, both, f.animator, f.scene, &collision);
    REQUIRE(f.triggers.opened(0));
    const f32 start = f.height();
    for (s32 tick = 0; tick < 30; ++tick) {
        f.triggers.update(1.0f / 30, both, f.animator, f.scene, &collision);
    }
    if (staysSolid) {
        CHECK(f.height() < start); // a lift carries its riders
        CHECK(collision.floorExitBlocked(0));
    } else {
        CHECK(f.height() == start); // the platform waits, and nobody is trapped on it
        CHECK_FALSE(collision.floorExitBlocked(0));
        CHECK_FALSE(f.triggers.settled(0));
    }
    // The rider gone, it goes on down.
    for (s32 tick = 0; tick < 30; ++tick) {
        f.triggers.update(1.0f / 30, button, f.animator, f.scene, &collision);
    }
    CHECK(f.height() < start);
}

TEST_CASE("revealed momentary bridges wait for their final rider without pressing the pad",
          "[game][world][triggers][bridge-occupants][triggers-local-rewind]") {
    const bool enemy = GENERATE(false, true);
    const s32 support = GENERATE(0, 1);
    const f32 step = GENERATE(1.0f / 30, 1.0f / 60);
    SwitchFixture f(R"({"info":0,"position":[20,0,0],
      "params":[0,0,16,0,2,255,0,0,0,0,0,0]})",
                    R"({"name":"WALL","position":[0,10,0],"flags":4096,"child":1},
                       {"name":"WALL","position":[0,2,0]})");
    WorldCollision collision;
    const std::array button{TriggerVisitor{.position = Vec3{20, 0, 0}, .party = 0}};
    const std::array rider{
        TriggerVisitor{.position = Vec3{0, 10, 0}, .floorObject = support, .party = 1}};
    const std::array floors{support};
    const auto update = [&](bool occupied) {
        f.triggers.update(step,
                          !enemy && occupied ? std::span<const TriggerVisitor>{rider}
                                             : std::span<const TriggerVisitor>{},
                          f.animator, f.scene, &collision,
                          enemy && occupied ? std::span<const s32>{floors}
                                            : std::span<const s32>{});
    };
    update(true); // An absent bridge cannot be summoned by a floor claim alone.
    CHECK_FALSE(f.triggers.opened(0));
    CHECK_FALSE(collision.solid(0));
    CHECK_FALSE(f.triggers.trigger(0).fired);
    f.triggers.update(1, button, f.animator, f.scene, &collision);
    REQUIRE(f.triggers.opened(0));
    REQUIRE(f.triggers.alphaOf(0) == 1);
    f.triggers.takeOpenings();
    f.triggers.takeSettled();
    f.triggers.takeCameraCues();
    for (s32 tick = 0; tick < 120; ++tick) {
        update(true);
        CHECK(f.triggers.opened(0));
        CHECK(f.triggers.alphaOf(0) == 1);
        CHECK(collision.solid(0));
        CHECK(collision.solid(1));
        CHECK_FALSE(f.triggers.trigger(0).fired);
    }
    CHECK(f.triggers.takeOpenings().empty());
    CHECK(f.triggers.takeCameraCues().empty());
    LevelTriggers::LocalSnapshot before;
    LevelTriggers::LocalSnapshot released;
    LevelTriggers::LocalSnapshot replayed;
    f.triggers.saveLocal(before);
    update(false); // Leaving, dying or losing support releases the bridge.
    CHECK_FALSE(f.triggers.opened(0));
    CHECK_FALSE(collision.solid(0));
    CHECK_FALSE(collision.solid(1));
    CHECK(f.triggers.alphaOf(0) < 1);
    f.triggers.saveLocal(released);
    REQUIRE(f.triggers.restoreLocal(before));
    update(false);
    f.triggers.saveLocal(replayed);
    CHECK(f.triggers.equalLocal(released, replayed));
}

TEST_CASE("floor occupants do not retain ordinary lifts or hide-on-use fields",
          "[game][world][triggers][bridge-occupants]") {
    const s32 kind = GENERATE(0, 48);
    SwitchFixture f(std::format(R"({{"info":0,"position":[20,0,0],
      "params":[0,0,{},0,2,255,0,0,0,0,20,0]}})",
                                kind));
    WorldCollision collision;
    const std::array button{TriggerVisitor{.position = Vec3{20, 0, 0}}};
    const std::array floors{0};
    f.triggers.update(1, button, f.animator, f.scene, &collision);
    REQUIRE(f.triggers.opened(0));
    f.triggers.update(1, {}, f.animator, f.scene, &collision, floors);
    CHECK_FALSE(f.triggers.opened(0));
    if (kind == 0) {
        CHECK(f.height() == Approx(10));
    } else {
        CHECK(f.triggers.alphaOf(0) == 1);
    }
}

TEST_CASE("D1 native two-player bridges retain players and enemy supports after pad release",
          "[game][world][triggers][bridge-occupants][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELD1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("D1");
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    world.setPlayerCount(2);
    world.startTriggers({});
    std::set<std::string> bridges;
    for (usize i = 0; i < world.triggers().size(); ++i) {
        const auto trigger = world.triggers().trigger(i);
        if (!trigger.enabled || trigger.chained || trigger.target < 0 || trigger.minPlayers != 1 ||
            !LevelTrigger::momentaryFade(trigger.kind)) {
            continue;
        }
        CAPTURE(trigger.instance, trigger.target, trigger.flags,
                world.layout().objects()[static_cast<usize>(trigger.target)].name);
        const std::array button{
            TriggerVisitor{.position = trigger.spot, .party = 0},
            TriggerVisitor{.position = trigger.spot + Vec3{1000, 0, 0}, .party = 1}};
        world.updateTriggers(1, button);
        REQUIRE(world.triggers().opened(trigger.target));
        REQUIRE(world.collision().solid(trigger.target));
        const std::array rider{TriggerVisitor{.position = trigger.spot + Vec3{1000, 0, 0},
                                              .floorObject = trigger.target,
                                              .party = 1}};
        world.updateTriggers(1, rider);
        CHECK(world.triggers().opened(trigger.target));
        CHECK(world.collision().solid(trigger.target));
        CHECK(world.triggers().alphaOf(trigger.target) == 1);
        const std::array enemyFloor{trigger.target};
        world.updateTriggers(1, {}, false, enemyFloor);
        CHECK(world.triggers().opened(trigger.target));
        CHECK(world.collision().solid(trigger.target));
        CHECK_FALSE(world.triggers().trigger(i).fired);
        world.updateTriggers(1, {});
        CHECK_FALSE(world.triggers().opened(trigger.target));
        CHECK_FALSE(world.collision().solid(trigger.target));
        CHECK(world.triggers().alphaOf(trigger.target) == 0);
        bridges.insert(world.layout().objects()[static_cast<usize>(trigger.target)].name);
    }
    const std::set<std::string> expected{"D1DRAWB1", "D1DRAWB4", "D1DRAWB66"};
    CHECK(bridges == expected);
}

// Dispatch-only coverage: synthetic visitors at the marker test party-size variants,
// but bypass collision and therefore do not certify route reachability.
TEST_CASE("isolated root-switch dispatch retains the catalogued ordinary switches",
          "[game][world][switch-census][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize levels = 0;
    usize switches = 0;
    usize activated = 0;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto ref = catalog.byName(name);
            REQUIRE(ref.has_value());
            const auto file = test::assetOrSkip(ref->directory + "/WORLDS.PS2");
            WorldLayout layout;
            REQUIRE(layout.load(file.parent_path()));
            ++levels;
            WorldAnimator animator;
            animator.bind(layout);
            LevelTriggers triggers;
            triggers.bind(layout, animator, nullptr);
            switches += triggers.size();
            for (usize i = 0; i < triggers.size(); ++i) {
                const auto trigger = triggers.trigger(i);
                CAPTURE(name, i, trigger.flags, trigger.target);
                if (trigger.chained ||
                    (trigger.flags & (LevelTrigger::kRequirement | LevelTrigger::kCloses)) != 0) {
                    continue;
                }
                for (s32 count = 1; count <= 4; ++count) {
                    animator.bind(layout);
                    triggers.bind(layout, animator, nullptr);
                    triggers.setPlayerCount(count);
                    std::vector<TriggerVisitor> party;
                    party.reserve(static_cast<usize>(count));
                    for (s32 player = 0; player < count; ++player) {
                        party.push_back(TriggerVisitor{.position = trigger.spot,
                                                       .floorObject = trigger.target,
                                                       .party = player});
                    }
                    WorldScene scene;
                    triggers.update(kStep, party, animator, scene, nullptr);
                    const bool eligible = trigger.minPlayers > 10 ? count == trigger.minPlayers - 10
                                                                  : count >= trigger.minPlayers;
                    CAPTURE(count, trigger.minPlayers);
                    CHECK(triggers.trigger(i).enabled == eligible);
                    CHECK(triggers.trigger(i).fired == eligible);
                    if (eligible && trigger.target >= 0) {
                        CHECK(triggers.opened(trigger.target));
                    }
                    activated += eligible ? 1 : 0;
                }
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
    // A second controller can already have moved/turned this node. Height updates
    // own only Y, including their initial placement and their settled ticks.
    const Mat4 turned = glm::rotate(glm::translate(Mat4{1}, Vec3{2, 10, -3}), 0.7f, Vec3{0, 1, 0});
    scene.setObjectTransform(0, turned);
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    triggers.placeTargets(scene, nullptr);
    CHECK(scene.localTransform(0) == turned);
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
    CHECK(scene.localTransform(0)[0] == turned[0]);
    CHECK(scene.localTransform(0)[1] == turned[1]);
    CHECK(scene.localTransform(0)[2] == turned[2]);
    CHECK(scene.localTransform(0)[3].x == turned[3].x);
    CHECK(scene.localTransform(0)[3].z == turned[3].z);
}

TEST_CASE("switch art missing from the level's own item archive comes from the realm's",
          "[triggers][trigger-visibility][trigger-archives]") {
    SwitchFixture f(
        R"({"info":0,"flags":0,"position":[0,0,0],"params":[0,0,0,0,4,255,7,0,0,0,100,0]})");
    const auto own = test::sampleLevel("trigger-own-archive");
    writeTextFile(own / "animations.json", R"({"trees":[{"name":"TENTACLE",
      "nodes":[{"name":"WALL","object":"WALL","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"OFF","frames":1,"rate":30}]}]})");
    const auto realm = test::sampleLevel("trigger-realm-archive");
    writeTextFile(realm / "animations.json", R"({"trees":[{"name":"BRIDGEPAD",
      "nodes":[{"name":"WALL","object":"WALL","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"OFF","frames":1,"rate":30}]}]})");
    ItemArchive ownItems;
    ItemArchive realmItems;
    REQUIRE(ownItems.load(own));
    REQUIRE(realmItems.load(realm));
    f.triggers.bindFigures(f.device, f.layout, AssetScope(ownItems));
    CHECK(f.triggers.figure(0) == nullptr);
    f.triggers.bindFigures(f.device, f.layout, AssetScope(ownItems, &realmItems));
    REQUIRE(f.triggers.figure(0) != nullptr);
    f.triggers.draw(f.device, Mat4{1}, {});
    CHECK(f.device.draws.size() == 1);
}

TEST_CASE("Castle Courtyard switches are visible despite the level's own item archive",
          "[triggers][trigger-visibility][assets]") {
    const auto root = test::assetOrSkip("WDATA/CASTLE.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("A1")));
    REQUIRE(world.items().trees.find("TENTACLE").has_value());
    REQUIRE_FALSE(world.items().trees.find("TRIGGER2").has_value());
    const auto& triggers = world.triggers();
    REQUIRE(triggers.size() == 8);
    usize shown = 0;
    for (usize i = 0; i < triggers.size(); ++i) {
        const auto& instance =
            world.layout().itemInstances()[static_cast<usize>(triggers.trigger(i).instance)];
        const bool markerOnly = (instance.flags & 2U) != 0;
        INFO("trigger " << i);
        CHECK((triggers.figure(i) != nullptr) == !markerOnly);
        shown += triggers.figure(i) != nullptr ? 1 : 0;
    }
    CHECK(shown == 6);
}

TEST_CASE("every catalogued level resolves the art of each of its visible switches",
          "[triggers][trigger-visibility][asset-conformance][assets]") {
    const auto root = test::assetOrSkip("WDATA/CASTLE.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize levels = 0;
    usize figures = 0;
    for (const auto& realm : catalog.realms()) {
        // The developer test realm places BIGTRIGGER, whose art only the unsupported demo
        // archive holds; every shipped realm must resolve all of its switches.
        if (realm.file == "TEST") {
            continue;
        }
        for (const std::string& name : realm.levels) {
            const auto ref = catalog.byName(name);
            REQUIRE(ref);
            test::FakeRenderDevice device;
            LevelWorld world;
            CAPTURE(name);
            REQUIRE(world.load(device, root, *ref));
            ++levels;
            const auto& triggers = world.triggers();
            for (usize i = 0; i < triggers.size(); ++i) {
                const auto& instance =
                    world.layout()
                        .itemInstances()[static_cast<usize>(triggers.trigger(i).instance)];
                if ((instance.flags & 2U) != 0) {
                    continue;
                }
                const auto& info = world.layout().itemInfos()[static_cast<usize>(instance.info)];
                CAPTURE(i, info.name, instance.position);
                CHECK(triggers.figure(i) != nullptr);
                figures += triggers.figure(i) != nullptr ? 1 : 0;
            }
        }
    }
    CHECK(levels >= 62); // the shipped realms, without the three test levels
    CHECK(figures > 0);
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
    f.triggers.bindFigures(f.device, f.layout, AssetScope(items));
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
          "[triggers][trigger-visibility][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    for (const std::string_view level : {"L1", "G1", "E1"}) {
        CAPTURE(level);
        test::assetOrSkip(std::format("LEVELS/LEVEL{}/WORLDS.PS2", level));
        test::assetOrSkip(std::format("ITEMS/LEVEL{}/ANIM.PS2", level.front()));
        test::FakeRenderDevice device;
        WorldLayout layout;
        REQUIRE(layout.load(root / "LEVELS" / std::format("LEVEL{}", level)));
        WorldAnimator animator;
        animator.bind(layout);
        ItemArchive items;
        REQUIRE(items.load(root / "ITEMS" / std::format("LEVEL{}", level.front())));
        LevelTriggers triggers;
        triggers.bind(layout, animator, nullptr);
        triggers.bindFigures(device, layout, AssetScope(items));
        triggers.draw(device, Mat4{1}, {});
        const usize actual = device.draws.size();
        device.draws.clear();
        usize hidden = 0;
        for (usize i = 0; i < triggers.size(); ++i) {
            const auto& instance =
                layout.itemInstances()[static_cast<usize>(triggers.trigger(i).instance)];
            if ((instance.flags & 2) != 0 || !triggers.trigger(i).enabled) {
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
          "[game][world][triggers][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("ITEMS/LEVELE/ANIM.PS2");
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELE1"));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    ItemArchive items;
    REQUIRE(items.load(root / "ITEMS/LEVELE"));
    triggers.bindFigures(device, layout, AssetScope(items));
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
          "[triggers][temple-gates][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("ITEMS/LEVELE/ANIM.PS2");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("E1");
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    world.startTriggers({});
    world.updateTriggers(0, {});
    // The two altar approach pads (110, 112) ride lift E1ELEV99, authored five under the
    // hall floor and out of reach from it (fn_8005F0F4's height rule) until the lever
    // (130) raises them through its chain. Each is touched at floor height, never by
    // teleporting to the marker.
    for (const s32 instance : {130, 110, 112, 159}) {
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
        if (instance == 130) {
            // The lift rises its five at four a second; the pads come up on its deck.
            for (s32 frame = 0; frame < 90; ++frame) {
                world.update(kStep);
                world.updateTriggers(kStep, {});
            }
        }
    }
}

TEST_CASE("lift pads activate from the lowered deck rather than their authored marker height",
          "[triggers][platform-contact][assets]") {
    const auto [name, instance] = GENERATE(std::pair{"G1", 418}, std::pair{"G1", 313},
                                           std::pair{"D4", 342}, std::pair{"J3", 483});
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
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

TEST_CASE("Ice Camp retains its pillar rotation and its final switch raises the runestone",
          "[triggers][rotators][ice-runestone][assets]") {
    const auto root = test::assetOrSkip("WDATA/ICE.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("I2")));
    world.setPlayerCount(1);
    world.startTriggers({});
    // The earlier switch behind the ice wall has already raised the approach bridge.
    world.activateTrigger(1, true);
    usize liftSwitch = world.triggers().size();
    for (usize i = 0; i < world.triggers().size(); ++i) {
        if (world.triggers().trigger(i).instance == 491) {
            liftSwitch = i;
        }
    }
    REQUIRE(liftSwitch < world.triggers().size());
    PlayerActor actor;
    const auto approachSwitch = [&] {
        actor.spawn(0, {}, nullptr, Vec3{-7.46875f, 26.3f, -24.9453125f}, 0);
        actor.settle(world.collision());
        for (s32 frame = 0; frame < 180 && !world.triggers().trigger(liftSwitch).fired; ++frame) {
            const Vec3 toward = world.triggers().trigger(liftSwitch).spot - actor.position();
            const Vec2 direction = glm::normalize(Vec2{toward.x, toward.z});
            actor.update(MoveInput{.direction = direction, .magnitude = 1}, 0, kStep,
                         &world.collision());
            actor.fall(kStep, world.collision());
            const std::array visitors{TriggerVisitor{.position = actor.position(),
                                                     .radius = actor.reach(), // as PlayScene has it
                                                     .height = actor.height()}};
            world.update(kStep);
            world.updateTriggers(kStep, visitors);
        }
    };
    const std::array onPad{TriggerVisitor{.position = Vec3{-32.09375f, 16.5447f, -35.28125f}}};
    for (s32 frame = 0; frame < 300; ++frame) {
        world.update(kStep);
        world.updateTriggers(kStep, frame == 0 ? std::span<const TriggerVisitor>{onPad}
                                               : std::span<const TriggerVisitor>{});
    }
    REQUIRE(world.layout().objects()[247].name == "I2ELEV43");
    REQUIRE(world.layout().objects()[248].name == "I2ELEV6");
    const auto& pillar = world.rotators().rotator(4);
    REQUIRE(pillar.object == 247);
    REQUIRE(pillar.done);
    CHECK(world.scene().worldTransform(247)[0].x == Approx(std::cos(pillar.turned)));
    CHECK(world.scene().worldTransform(247)[0].z == Approx(-std::sin(pillar.turned)));
    REQUIRE(world.collision().objectTransform(247));
    CHECK(*world.collision().objectTransform(247) == world.scene().worldTransform(247));

    REQUIRE_FALSE(world.triggers().trigger(liftSwitch).fired);
    const auto before = world.collision().floorAt(Vec3{-16.3f, 42, -28.5f}, 0, 20);
    REQUIRE(before);
    REQUIRE(before->object == 248);
    // Walk from the approach bridge to the final switch instead of activating it directly.
    approachSwitch();
    CAPTURE(actor.position().x, actor.position().y, actor.position().z);
    REQUIRE(world.triggers().trigger(liftSwitch).fired);
    for (s32 frame = 0; frame < 90; ++frame) {
        world.update(kStep);
        world.updateTriggers(kStep, {});
    }
    CHECK(world.triggers().opened(248));
    const auto raised = world.collision().floorAt(Vec3{-16.3f, 42, -28.5f}, 0, 20);
    REQUIRE(raised);
    CHECK(raised->object == 248);
    CHECK(raised->y == Approx(before->y + 3.5f));
    CHECK(world.scene().worldTransform(247)[0].x == Approx(std::cos(pillar.turned)));

    actor.spawn(0, {}, nullptr, Vec3{-16.3f, 41.2f, -24}, 0);
    actor.settle(world.collision());
    bool tookRune = false;
    for (s32 frame = 0; frame < 60 && !tookRune; ++frame) {
        actor.update(MoveInput{.direction = Vec2{0, -1}, .magnitude = 1}, 0, kStep,
                     &world.collision());
        actor.fall(kStep, world.collision());
        const std::array collectors{Collector{actor.position(), actor.radius(), actor.height()}};
        for (const auto& pickup : world.collect(device, collectors)) {
            tookRune |= world.placedItems().item(pickup.item).instance == 505;
        }
    }
    CAPTURE(actor.position().x, actor.position().y, actor.position().z);
    CHECK(tookRune);
}

TEST_CASE("Ice Camp spinning platforms retain their switch-controlled height",
          "[triggers][rotators][ice-runestone][assets]") {
    const s32 instance = GENERATE(480, 471, 472, 473);
    const auto root = test::assetOrSkip("WDATA/ICE.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("I2")));
    world.startTriggers({});
    usize index = world.triggers().size();
    for (usize i = 0; i < world.triggers().size(); ++i) {
        if (world.triggers().trigger(i).instance == instance) {
            index = i;
        }
    }
    REQUIRE(index < world.triggers().size());
    const auto& trigger = world.triggers().trigger(index);
    const auto object = static_cast<usize>(trigger.target);
    const f32 initialHeight = world.scene().worldTransform(object)[3].y;
    const f32 raisedHeight = world.layout().worldPosition(object).y;
    REQUIRE(initialHeight < raisedHeight);
    for (s32 frame = 0; frame < 60; ++frame) {
        world.update(kStep);
        world.updateTriggers(kStep, {});
    }
    CHECK(world.scene().worldTransform(object)[3].y == Approx(initialHeight));
    CHECK(world.scene().worldTransform(object)[0].z != Approx(0));
    const std::array visitors{TriggerVisitor{.position = trigger.spot}};
    world.updateTriggers(kStep, visitors);
    REQUIRE(trigger.fired);
    CHECK(world.scene().worldTransform(object)[3].y > initialHeight);
    CHECK(world.scene().worldTransform(object)[3].y < raisedHeight);
    for (s32 frame = 0; frame < 90; ++frame) {
        world.update(kStep);
        world.updateTriggers(kStep, {});
    }
    CHECK(world.scene().worldTransform(object)[3].y == Approx(raisedHeight));
    REQUIRE(world.collision().objectTransform(trigger.target));
    CHECK(*world.collision().objectTransform(trigger.target) ==
          world.scene().worldTransform(object));
}

TEST_CASE("the Mine's lift is walkable across its green centre, which is a child of its ring",
          "[game][world][triggers][ice-lift][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELI3/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("I3");
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const auto& objects = world.layout().objects();
    const auto indexOf = [&](std::string_view name) {
        for (usize i = 0; i < objects.size(); ++i) {
            if (objects[i].name == name) {
                return static_cast<s32>(i);
            }
        }
        return -1;
    };
    const s32 ring = indexOf("I3ELEV6");
    const s32 centre = indexOf("I3NS_CYLINDER46");
    REQUIRE(ring >= 0);
    REQUIRE(centre >= 0);
    const Vec3 lift = world.layout().worldPosition(static_cast<usize>(ring));
    // The ring's walkable strip is half a unit wide; the shallow cone within it, raised
    // 2.3 units in the ring's space, carries the rest of the platform.
    const auto middle = world.collision().floorAt(lift + Vec3{0, 2.0f, 0}, 3.0f, 4.0f);
    REQUIRE(middle.has_value());
    CHECK(middle->object == centre);
    CHECK(middle->y == Approx(lift.y + 2.3f + 0.5f).margin(0.2f));
    const auto halfway = world.collision().floorAt(lift + Vec3{2.5f, 2.0f, 1.5f}, 3.0f, 4.0f);
    REQUIRE(halfway.has_value());
    CHECK(halfway->object == centre);
    CHECK(halfway->y < middle->y);
}

TEST_CASE("Maze invisible platforms respond to floor contact", "[maze-triggers][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELJ4/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("J4");
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    world.startTriggers({});
    usize tested = 0;
    for (usize i = 0; i < world.triggers().size(); ++i) {
        const auto trigger = world.triggers().trigger(i);
        if (trigger.instance < 329 || trigger.instance > 332) {
            continue;
        }
        const auto floor = world.collision().floorAt(trigger.spot, 4, 10);
        CAPTURE(trigger.instance, trigger.spot.x, trigger.spot.y, trigger.spot.z, trigger.floor,
                trigger.target, trigger.chained);
        REQUIRE(floor);
        CAPTURE(floor->y, floor->object);
        CHECK(trigger.floor == floor->object);
        const std::array visitors{
            TriggerVisitor{.position = Vec3{trigger.spot.x, floor->y, trigger.spot.z},
                           .floorObject = floor->object}};
        world.updateTriggers(kStep, visitors);
        CHECK(world.triggers().trigger(i).fired);
        ++tested;
    }
    CHECK(tested == 4);
}

TEST_CASE("leaving the Maze fountain raises the approach trigger with its first platform",
          "[maze-triggers][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELJ4/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("J4")));
    world.startTriggers({});
    const auto indexOf = [&](s32 instance) {
        for (usize i = 0; i < world.triggers().size(); ++i) {
            if (world.triggers().trigger(i).instance == instance) {
                return i;
            }
        }
        return world.triggers().size();
    };
    const usize fountain = indexOf(417);
    const usize approach = indexOf(328);
    REQUIRE(fountain < world.triggers().size());
    REQUIRE(approach < world.triggers().size());
    REQUIRE(world.triggers().trigger(approach).floor == 7); // J4A1 starts 43 units down.
    const f32 initial = world.triggers().trigger(approach).spot.y;
    const std::array visitors{TriggerVisitor{.position = world.triggers().trigger(fountain).spot}};
    world.updateTriggers(kStep, visitors);
    REQUIRE(world.triggers().opened(7));
    for (s32 frame = 0; frame < 180; ++frame) {
        world.update(kStep);
        world.updateTriggers(kStep, {});
    }
    const auto trigger = world.triggers().trigger(approach);
    CHECK(trigger.spot.y == Approx(initial + 43).margin(0.02f));
    PlayerActor actor;
    actor.spawn(0, {}, nullptr, trigger.spot + Vec3{0, 0, -4}, 0);
    actor.settle(world.collision());
    for (s32 frame = 0; frame < 30; ++frame) {
        actor.update(MoveInput{.direction = Vec2{0, 1}, .magnitude = 1}, 0, kStep,
                     &world.collision());
        const std::array walking{
            TriggerVisitor{.position = actor.position(), .radius = actor.radius()}};
        world.update(kStep);
        world.updateTriggers(kStep, walking);
    }
    CHECK(world.triggers().trigger(approach).fired);
    CHECK(world.triggers().opened(298)); // J4A2 is the next section that falls into place.
}

TEST_CASE("catalogued platform pads respond at their actual supporting surface",
          "[platform-census][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize tested = 0;
    std::set<s32> subtypes;
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto ref = catalog.byName(name);
            REQUIRE(ref);
            test::assetOrSkip(ref->directory + "/WORLDS.PS2");
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
                const s32 count = trigger.minPlayers > 10 ? trigger.minPlayers - 10
                                                          : std::max(1, trigger.minPlayers);
                triggers.setPlayerCount(count);
                std::vector<TriggerVisitor> visitors;
                visitors.reserve(static_cast<usize>(count));
                for (s32 player = 0; player < count; ++player) {
                    visitors.push_back(
                        TriggerVisitor{.position = Vec3{trigger.spot.x, floor->y, trigger.spot.z},
                                       .floorObject = floor->object,
                                       .party = player});
                }
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

// What the shipped triggers ask of their targets. No trigger's flags name two latches, so
// fireNode's order among the latch bits never has to choose between them. Each target moves by
// its keyed track, a fade or its height, but for two keyed targets whose pads fade them: both
// their track and their fade run.
TEST_CASE("shipped triggers name one latch at most, and only two keyed targets also fade",
          "[trigger-census][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    usize levels = 0;
    usize triggers = 0;
    // The latch each trigger's flags select, in fireNode's order.
    usize closes = 0;
    usize opensOnce = 0;
    usize toggles = 0;
    usize momentary = 0;
    // What moves each target, its keyed track first, as openTarget asks.
    usize keyed = 0;
    usize fading = 0;
    usize rising = 0;
    std::set<std::string> fadingKeyedPads;    // level, trigger instance and target object
    std::set<std::string> fadingKeyedTargets; // level and object of a stored keyed fade
    for (const auto& realm : catalog.realms()) {
        for (const auto& name : realm.levels) {
            const auto ref = catalog.byName(name);
            REQUIRE(ref);
            test::assetOrSkip(ref->directory + "/WORLDS.PS2");
            WorldLayout layout;
            REQUIRE(layout.load(root / ref->directory));
            ++levels;
            WorldAnimator preview;
            preview.bind(layout);
            LevelTriggers inventory;
            inventory.bind(layout, preview, nullptr);
            const auto objectName = [&](s32 object) {
                return layout.objects()[static_cast<usize>(object)].name;
            };
            for (usize i = 0; i < inventory.size(); ++i) {
                const LevelTrigger& trigger = inventory.trigger(i);
                CAPTURE(name, trigger.instance, trigger.target, trigger.flags);
                ++triggers;
                CHECK(std::popcount(trigger.flags & LevelTrigger::kLatches) <= 1);
                if ((trigger.flags & LevelTrigger::kCloses) != 0) {
                    ++closes;
                } else if ((trigger.flags & LevelTrigger::kOpensOnce) != 0) {
                    ++opensOnce;
                } else if ((trigger.flags & LevelTrigger::kToggles) != 0) {
                    ++toggles;
                } else {
                    ++momentary;
                }
                // Every trigger that drives a target, not only the first, which gives the
                // target its kind.
                if (trigger.target >= 0 && preview.trackOf(trigger.target).has_value() &&
                    (trigger.kind & LevelTrigger::kFades) != 0) {
                    fadingKeyedPads.insert(std::format("{} {} {}", name, trigger.instance,
                                                       objectName(trigger.target)));
                }
            }
            for (const auto& target : inventory.targets()) {
                const bool fades = (target.kind & LevelTrigger::kFades) != 0;
                if (target.animated && fades) {
                    fadingKeyedTargets.insert(
                        std::format("{} {}", name, objectName(target.object)));
                }
                if (target.animated) {
                    ++keyed;
                } else if (fades) {
                    ++fading;
                } else {
                    ++rising;
                }
            }
        }
    }
    CHECK(levels == 65);
    CHECK(triggers == 1424);
    CHECK(closes == 163);
    CHECK(opensOnce == 1130);
    CHECK(toggles == 64);
    CHECK(momentary == 67);
    CHECK(keyed == 472);
    CHECK(fading == 20);
    CHECK(rising == 514);
    CHECK(fadingKeyedPads == std::set<std::string>{"L1 205 L1LITPODIUM", "S1 117 S1DRAWBKJ"});
    CHECK(fadingKeyedTargets == std::set<std::string>{"L1 L1LITPODIUM", "S1 S1DRAWBKJ"});
}

TEST_CASE("Underworld switch artwork remains visible before and after trigger contact",
          "[triggers][underworld-switches][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELF1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("ITEMS/LEVELF/ANIM.PS2");
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELF1"));
    ItemArchive items;
    REQUIRE(items.load(root / "ITEMS/LEVELF"));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    triggers.bindFigures(device, layout, AssetScope(items));
    REQUIRE(triggers.size() == 11);
    triggers.setPlayerCount(3);
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
          "[game][world][triggers][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELC3/WORLDS.PS2").parent_path().parent_path().parent_path();
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
    triggers.setPlayerCount(2);
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
    // Both targets finish once, including the chained target with no height travel.
    f.triggers.update(kStep, party, f.animator, f.scene, &f.collision);
    const std::vector<TriggerOpening> settled = f.triggers.takeSettled();
    REQUIRE(settled.size() == 2);
    CHECK(std::ranges::count(settled, 8, &TriggerOpening::target) == 1);
    CHECK(std::ranges::count(settled, 9, &TriggerOpening::target) == 1);
    for (const auto& target : settled) {
        CHECK_FALSE(target.fades);
    }
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

/** A Knight's record: the class width every class shares. */
ClassStats knightStats() {
    ClassStats stats;
    stats.speedMin = 350.0f;
    stats.speedMax = 750.0f;
    stats.height = 5.0f;
    stats.width = 1.5f;
    stats.collisionY = 2.5f;
    return stats;
}

/** Walks `actor` at a trigger's spot for up to `ticks`, the trigger seeing the body as
 * PlayScene shows it: at the step's aim. True once the trigger fires. */
bool walkToPull(PlayerActor& actor, LevelTriggers& triggers, const LevelTrigger& trigger,
                WorldAnimator& animator, WorldScene& scene, WorldCollision& collision, s32 ticks) {
    for (s32 tick = 0; tick < ticks && !trigger.fired; ++tick) {
        const Vec3 away = trigger.spot - actor.position();
        actor.update(MoveInput{glm::normalize(Vec2{away.x, away.z}), 1.0f}, 0.0f, kStep,
                     &collision);
        std::array party{TriggerVisitor{.position = actor.stepAim(),
                                        .radius = actor.reach(),
                                        .floorObject = actor.floorObject(),
                                        .party = 0,
                                        .height = actor.height()}};
        triggers.update(kStep, party, animator, scene, &collision);
    }
    return trigger.fired;
}

TEST_CASE("the Branches' lever rests on the bridge under it, not the ledge beside it",
          "[triggers][assets]") {
    const auto level = test::assetOrSkip("LEVELS/LEVELD4/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(level));
    WorldCollision collision;
    REQUIRE(collision.load(level, layout));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, &collision);
    usize lever = triggers.size();
    for (usize i = 0; i < triggers.size(); ++i) {
        if (triggers.trigger(i).instance == 352) {
            lever = i;
        }
    }
    REQUIRE(lever < triggers.size());
    // Instance 352 is authored on the bridge at -13.80; a ledge's edge within a unit stands
    // at -10, and the lever was lifted onto it, floating beside the torch.
    CHECK(triggers.trigger(lever).spot.y == Approx(-13.8f + 0.1f).margin(0.05f));
    triggers.clear();
}

TEST_CASE("the Fissure's trap-wall lever is pulled from the rock its handle stands in",
          "[triggers][assets]") {
    const auto level = test::assetOrSkip("LEVELS/LEVELI4/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(level));
    WorldCollision collision;
    REQUIRE(collision.load(level, layout));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, &collision);
    usize lever = triggers.size();
    for (usize i = 0; i < triggers.size(); ++i) {
        if (triggers.trigger(i).instance == 534) {
            lever = i;
        }
    }
    REQUIRE(lever < triggers.size());
    const LevelTrigger& trigger = triggers.trigger(lever);
    REQUIRE(trigger.target == 1261); // I4TRAPW8
    // The spot lies 0.7 inside rock line 2644, whose face holds the wall sphere a width
    // out: 2.22 from the spot, past the 2.2 its pad and the class width reach together.
    const ClassStats stats = knightStats();
    PlayerActor actor;
    actor.spawn(0, {}, &stats, Vec3{-11.6f, -41.8f, 189.6f}, 0.0f);
    actor.settle(collision);
    WorldScene scene;
    CHECK(walkToPull(actor, triggers, trigger, animator, scene, collision, 90));
    const Vec3 at = actor.position();
    CHECK(std::hypot(trigger.spot.x - at.x, trigger.spot.z - at.z) >
          trigger.radius + actor.reach());
    triggers.clear();
}

TEST_CASE("the Nightmare's swinging platform brings its lever and the gargoyle piece up on deck",
          "[triggers][placed-items][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELJ6/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("J6");
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    // J6ROTELE02's track starts ten units down and on end in a wall slot, rises, swings flat
    // and settles at its authored height. The lever (instance 592) and the gargoyle piece
    // (GARGLION) are authored on the deck at rest; attached in the first frame's pose they
    // would swing out five units beside the deck and end on their sides.
    const PlacedItems& items = world.placedItems();
    usize claw = items.size();
    for (usize i = 0; i < items.size(); ++i) {
        if (items.item(i).name == "GARGLION") {
            claw = i;
        }
    }
    REQUIRE(claw < items.size());
    const auto indexOf = [&](s32 instance) {
        for (usize i = 0; i < world.triggers().size(); ++i) {
            if (world.triggers().trigger(i).instance == instance) {
                return i;
            }
        }
        return world.triggers().size();
    };
    const usize call = indexOf(590);
    const usize onDeck = indexOf(592);
    REQUIRE(call < world.triggers().size());
    REQUIRE(onDeck < world.triggers().size());
    REQUIRE(items.item(claw).floor);
    CHECK(world.triggers().trigger(onDeck).floor == 189); // J6ELEWALK#1, the deck's walk
    const Vec3 spot = world.triggers().trigger(call).spot;
    std::vector<TriggerVisitor> party{TriggerVisitor{.position = spot, .party = 0}};
    for (s32 tick = 0; tick < 240; ++tick) {
        world.update(kStep, false);
        world.updateTriggers(kStep, party);
        if (tick == 0) {
            party[0].position = spot + Vec3{1000, 0, 0};
        }
    }
    const auto& piece = items.item(claw);
    CHECK(glm::distance(piece.position, Vec3{152.69f, 64.1f, 51.82f}) < 0.25f);
    CHECK(piece.transform[1].y == Approx(1.0f).margin(0.01f));
    const LevelTrigger& lever = world.triggers().trigger(onDeck);
    CHECK(glm::distance(lever.spot, Vec3{154.98f, 64.1f, 51.76f}) < 0.25f);
    CHECK(lever.placement[1].y == Approx(1.0f).margin(0.01f));
}

TEST_CASE(
    "E1's door pads ride the lift its lever raises, out of reach from the hall floor till then",
    "[triggers][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("E1");
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    const auto indexOf = [&](s32 instance) {
        for (usize i = 0; i < world.triggers().size(); ++i) {
            if (world.triggers().trigger(i).instance == instance) {
                return i;
            }
        }
        return world.triggers().size();
    };
    // TRIGGER3 (instance 130) is the lever; its chain fires BRIDGEPAD 12, which raises the
    // lift E1ELEV99. The door pads (110 and 112, for E1OPEN3 and E1OPEN4) stand on the
    // lift's deck, authored five under the hall floor, which lies at nought over them.
    const usize lever = indexOf(130);
    const usize pad = indexOf(110);
    REQUIRE(lever < world.triggers().size());
    REQUIRE(pad < world.triggers().size());
    constexpr s32 kLift = 627;
    constexpr s32 kDoor3 = 625;
    constexpr s32 kDoor4 = 626;
    CHECK(world.triggers().trigger(pad).floor == 628); // E1FLOOR#19, the deck
    CHECK(world.triggers().trigger(pad).spot.y == Approx(-5.0f).margin(0.15f));
    const Vec3 overPads{0.07f, 0.0f, -77.5f};
    REQUIRE(world.collision().floorAt(overPads, 1.0f, 1.0f).has_value());
    std::vector<TriggerVisitor> party{
        TriggerVisitor{.position = overPads, .radius = 1.5f, .party = 0, .height = 5.0f}};
    const auto run = [&](s32 ticks) {
        for (s32 tick = 0; tick < ticks; ++tick) {
            world.update(kStep, false);
            world.updateTriggers(kStep, party);
        }
    };
    // Crossing the hall over the sunken pads, a body stands five above them: more than
    // their height and one.
    run(60);
    CHECK_FALSE(world.triggers().opened(kDoor3));
    CHECK_FALSE(world.triggers().opened(kDoor4));
    CHECK_FALSE(world.triggers().opened(kLift));
    // The lever raises the lift, and the deck brings the pads up flush with the hall.
    party[0].position = world.triggers().trigger(lever).spot;
    run(1);
    CHECK(world.triggers().opened(kLift));
    party[0].position = overPads + Vec3{1000, 0, 0};
    run(180);
    CHECK(world.triggers().settled(kLift));
    CHECK(world.triggers().trigger(pad).spot.y == Approx(0.0f).margin(0.15f));
    party[0].position = overPads;
    run(2);
    CHECK(world.triggers().opened(kDoor3));
    CHECK(world.triggers().opened(kDoor4));
}

TEST_CASE("every trigger subtype takes the item setup's kind flags, whatever the instance adds",
          "[triggers][trigger-kinds]") {
    // SetItem (items.c 6877-6891): subtypes 20-23 and 25-29 carry their kind's own flags, any
    // other the instance's flag word with 8 added; the word's high byte is always kept. The
    // word is read unsigned (zero-extended), as LevelTriggers reads it.
    struct Row {
        s32 subtype;
        u32 flags;
    };
    constexpr std::array<Row, 9> kOwnFlags{{{20, 0x10},
                                            {21, 0x8},
                                            {22, 0x12},
                                            {23, 0xA},
                                            {25, 0x804},
                                            {26, 0x2},
                                            {27, 0x80C},
                                            {28, 0x9},
                                            {29, 0xA}}};
    constexpr std::array<s32, 17> kSubtypes{-1, 0,  19, 20, 21, 22, 23, 24, 25,
                                            26, 27, 28, 29, 30, 31, 32, 255};
    constexpr std::array<u32, 11> kWords{0x0000, 0x0001, 0x0040, 0x00FF, 0x0400, 0x0800,
                                         0x1000, 0x2000, 0x8000, 0xFF00, 0xFFFF};
    std::string infos;
    std::string instances;
    for (usize s = 0; s < kSubtypes.size(); ++s) {
        infos += std::format(R"({}{{"type":5,"subtype":{},"name":"BRIDGEPAD","radius":1}})",
                             s == 0 ? "" : ",", kSubtypes[s]);
        for (const u32 word : kWords) {
            // No target (255, 255), the word little-endian in bytes 2 and 3, radius byte 2.
            instances += std::format(
                R"({}{{"info":{},"position":[0,0,0],"params":[255,255,{},{},2,255,0,0,0,0,0,0]}})",
                instances.empty() ? "" : ",", s, word & 0xFFU, word >> 8U);
        }
    }
    const auto dir = test::sampleLevel("trigger-kind-flags");
    writeTextFile(dir / "world.json",
                  std::format(R"({{"objects":[{{"name":"WALL","position":[0,10,0]}}],)"
                              R"("itemInfos":[{}],"itemInstances":[{}]}})",
                              infos, instances));
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr); // no collision: nothing can add the on-target bit
    REQUIRE(triggers.size() == kSubtypes.size() * kWords.size());
    for (usize s = 0; s < kSubtypes.size(); ++s) {
        for (usize w = 0; w < kWords.size(); ++w) {
            const u32 word = kWords[w];
            u32 own = word | 0x8U;
            for (const Row& row : kOwnFlags) {
                if (row.subtype == kSubtypes[s]) {
                    own = row.flags;
                }
            }
            const u32 expected = own | (word & 0xFF00U);
            const LevelTrigger& trigger = triggers.trigger((s * kWords.size()) + w);
            CAPTURE(kSubtypes[s], word);
            CHECK(trigger.flags == expected);
            CHECK(trigger.kind == (expected & 0xFFU));
        }
    }
}

TEST_CASE("a trigger latches by its first latch bit: closing, then opening once, then toggling",
          "[game][world][triggers][trigger-latch]") {
    // Closing comes before opening once, and opening once before toggling; with none of the
    // three a trigger is momentary, whatever else its word holds.
    constexpr u32 kOthers = ~LevelTrigger::kLatches;
    STATIC_REQUIRE(LevelTrigger::latchOf(0) == TriggerLatch::Momentary);
    STATIC_REQUIRE(LevelTrigger::latchOf(kOthers) == TriggerLatch::Momentary);
    STATIC_REQUIRE(LevelTrigger::latchOf(LevelTrigger::kCloses) == TriggerLatch::Closes);
    STATIC_REQUIRE(LevelTrigger::latchOf(LevelTrigger::kOpensOnce) == TriggerLatch::OpensOnce);
    STATIC_REQUIRE(LevelTrigger::latchOf(LevelTrigger::kToggles) == TriggerLatch::Toggles);
    STATIC_REQUIRE(LevelTrigger::latchOf(LevelTrigger::kCloses | LevelTrigger::kOpensOnce) ==
                   TriggerLatch::Closes);
    STATIC_REQUIRE(LevelTrigger::latchOf(LevelTrigger::kCloses | LevelTrigger::kToggles) ==
                   TriggerLatch::Closes);
    STATIC_REQUIRE(LevelTrigger::latchOf(LevelTrigger::kOpensOnce | LevelTrigger::kToggles) ==
                   TriggerLatch::OpensOnce);
    STATIC_REQUIRE(LevelTrigger::latchOf(LevelTrigger::kLatches | kOthers) == TriggerLatch::Closes);
    STATIC_REQUIRE(LevelTrigger::latchOf(LevelTrigger::kToggles | kOthers) ==
                   TriggerLatch::Toggles);
    // A trigger reads its whole flag word, the bits above its kind with it.
    LevelTrigger trigger;
    trigger.flags = LevelTrigger::kToggles | LevelTrigger::kWholeParty;
    CHECK(trigger.latch() == TriggerLatch::Toggles);
    trigger.flags = LevelTrigger::kWholeParty;
    CHECK(trigger.latch() == TriggerLatch::Momentary);
}

TEST_CASE("a momentary fade fades with no latch, requirement or oscillation to hold it",
          "[game][world][triggers][trigger-latch]") {
    // Of the low byte's 256 kinds, only the fades that at most stay solid or keep contact.
    constexpr usize kMomentaryKinds = [] {
        usize count = 0;
        for (u32 kind = 0; kind <= LevelTrigger::kKindMask; ++kind) {
            if (LevelTrigger::momentaryFade(kind)) {
                ++count;
            }
        }
        return count;
    }();
    STATIC_REQUIRE(kMomentaryKinds == 4);
    STATIC_REQUIRE(LevelTrigger::momentaryFade(LevelTrigger::kFades));
    STATIC_REQUIRE(LevelTrigger::momentaryFade(LevelTrigger::kFades | LevelTrigger::kStaysSolid));
    STATIC_REQUIRE(LevelTrigger::momentaryFade(LevelTrigger::kFades | LevelTrigger::kKeepContact));
    STATIC_REQUIRE(LevelTrigger::momentaryFade(LevelTrigger::kFades | LevelTrigger::kStaysSolid |
                                               LevelTrigger::kKeepContact));
    // The latched bridge's own kind, a crystal-gated fade and a fade that hides at a touch.
    STATIC_REQUIRE_FALSE(
        LevelTrigger::momentaryFade(LevelTrigger::kFades | LevelTrigger::kOpensOnce));
    STATIC_REQUIRE_FALSE(
        LevelTrigger::momentaryFade(LevelTrigger::kFades | LevelTrigger::kRequirement));
    STATIC_REQUIRE_FALSE(
        LevelTrigger::momentaryFade(LevelTrigger::kFades | LevelTrigger::kOscillates));
    STATIC_REQUIRE_FALSE(LevelTrigger::momentaryFade(LevelTrigger::kStaysSolid));
}

} // namespace
