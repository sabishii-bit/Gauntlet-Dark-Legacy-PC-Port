#include <array>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/CombatantFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    test::FakeRenderDevice device;
    test::CombatantFixture fighter;
    explicit Fixture(bool animated = false) {
        const auto root = test::scratchDirectory("combatant-nodes");
        const auto archive = root / "MONSTERS/GARM";
        std::filesystem::create_directories(root / "critter");
        std::filesystem::create_directories(archive);
        writeTextFile(archive / "body.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeTextFile(archive / "broken.obj", "v 0 0 0\nv 0.5 0 0\nv 0 0.5 0\nf 1 2 3\n");
        writeTextFile(archive / "objects.json", R"({"objects":[
          {"name":"BODY","file":"body.obj"},{"name":"GARMDARM","file":"broken.obj"}]})");
        writeFile(archive / "skin.png", test::kTinyPng);
        writeTextFile(archive / "textures.json", R"({"bitmaps":[
          {"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
        writeTextFile(archive / "animations.json", R"({"trees":[{"name":"GARM","nodes":[
          {"name":"ROOT","object":"BODY","position":[0,0,0],"parent":-1},
          {"name":"ARM","object":"BODY","position":[5,0,0],"parent":0},
          {"name":"HAND","object":"BODY","position":[2,0,0],"parent":1}],
          "sequences":[{"name":"IDLE","frames":3},{"name":"ATTACK","frames":3}]}]})");
        if (animated) {
            auto animation = nlohmann::json::parse(readTextFile(archive / "animations.json"));
            auto& idle = animation["trees"][0]["sequences"][0];
            idle["frames"] = 12;
            idle["tracks"] = nlohmann::json::parse(R"([
                {"node":0,"flags":16,"frames":[0,10],"values":[0,10]},
                {"node":1,"flags":32,"frames":[0,10],"values":[0,10]}])");
            writeTextFile(archive / "animations.json", animation.dump());
        }
        writeTextFile(root / "critter/GARM.json", R"({"descriptors":[{"prefix":"GARM","type":4}],
          "types":[{"moveCount":2,"maxHealth":100,"colCount":2}],
          "moves":[{"name":"READY","type":32,"anim":"IDLE"},
          {"name":"HIT","type":128,"anim":"ATTACK","colnode":"HAND","flags":16}],
          "nodes":[{"nodeName":"ARM","healthScale":0.2,"damageScale":1,"radius":2,
                    "flags":14,"sfxIndex":0},
                   {"nodeName":"HAND","healthScale":0.1,"damageScale":0.5,"radius":1,"flags":8}],
          "damages":[{"type":1,"sfxIndex":0,"minSpeed":25,"maxSpeed":25}],
          "sounds":[{"name":"BREAK"}]})");
        fighter.open(device, root, nullptr, {}, 'G');
        REQUIRE(fighter.spawn("GARM", Vec3{0}, 0));
    }
    void hit(f32 amount, s32 node, u32 flags = 0) {
        EnemyHit hit;
        hit.damage = amount;
        hit.node = node;
        hit.flags = flags;
        fighter.actor.hurt(hit);
    }
};

TEST_CASE("node hits cap local health, break once, remove descendants and disable bound moves",
          "[combatant-nodes][garm]") {
    Fixture f;
    REQUIRE(f.fighter.assets.brokenModels.size() == 1);
    const auto initial = f.fighter.actor.bodyTargets();
    REQUIRE(initial.size() == 3);
    REQUIRE(initial[0].node == 0);
    REQUIRE(initial[1].node == 1);
    REQUIRE(initial[2].node == -1);
    f.hit(25, 0);
    REQUIRE(f.fighter.actor.health() == Approx(80));
    const auto shots = f.fighter.actor.takeShots();
    REQUIRE(shots.size() == 1);
    REQUIRE(shots[0].damageIndex == 0);
    REQUIRE(shots[0].origin == Vec3{5, 0, 0});
    REQUIRE(f.fighter.actor.bodyTargets().size() == 1);
    f.hit(25, 0);
    f.hit(25, 1);
    REQUIRE(f.fighter.actor.health() == Approx(80));
    REQUIRE(f.fighter.actor.takeShots().empty());
    const std::array<EnemyView, 1> players{{{0, {0, 0, 5}, 1, 6}}};
    for (s32 i = 0; i < 30; ++i) {
        f.fighter.update(2, 1.0f / 30, players);
        REQUIRE(f.fighter.actor.moveName() != "HIT");
    }
    f.fighter.actor.draw(f.device, Mat4{1}, {});
    REQUIRE(f.device.draws.size() == 2); // root plus replacement, no hand
    REQUIRE(f.device.draws.back().vertices[1].position.x == Approx(5.5f));
}

TEST_CASE("node health preserves exact-boundary and heavy-hit bypass rules",
          "[combatant-nodes][garm]") {
    Fixture f;
    SECTION("exact budget does not trigger the overrun break") {
        f.hit(20, 0);
        REQUIRE(f.fighter.actor.takeShots().empty());
        f.hit(1, 0);
        REQUIRE(f.fighter.actor.takeShots().empty());
        REQUIRE(f.fighter.actor.health() == Approx(80));
    }
    SECTION("heavy damage bypasses the node's budget") {
        f.hit(25, 0, EnemyHit::kMagic);
        REQUIRE(f.fighter.actor.health() == Approx(75));
        REQUIRE(f.fighter.actor.takeShots().empty());
        REQUIRE(f.fighter.actor.bodyTargets().size() == 3);
    }
    SECTION("node damage scale precedes its budget") {
        f.hit(10, 1);
        REQUIRE(f.fighter.actor.health() == Approx(95));
    }
}

TEST_CASE("a broken part holds its local pose while its parent continues animating",
          "[combatant-nodes][garm]") {
    Fixture intact{true};
    Fixture broken{true};
    broken.hit(25, 0);
    intact.fighter.update(2, 1.0f / 30, {});
    broken.fighter.update(2, 1.0f / 30, {});
    const auto before = intact.fighter.actor.nodeTransform("ARM");
    const auto after = broken.fighter.actor.nodeTransform("ARM");
    REQUIRE(before.has_value());
    REQUIRE(after.has_value());
    REQUIRE((*before)[3].y > 0);
    REQUIRE((*after)[3].y == Approx(0));
    REQUIRE((*after)[3].x == Approx((*before)[3].x));
    REQUIRE((*after)[3].x > 5);
}

TEST_CASE("a light hit flashes only its collision mesh and expires without leaking to siblings",
          "[combatant-nodes][garm]") {
    Fixture f;
    test::FakeTexture white{1, 1};
    f.hit(1, 0);
    f.fighter.actor.draw(f.device, Mat4{1}, {}, nullptr, nullptr, &white);
    REQUIRE(f.device.draws.size() == 3);
    REQUIRE(f.device.draws[0].state.maskedTexture == nullptr);
    REQUIRE(f.device.draws[1].state.maskedTexture == &white);
    REQUIRE(f.device.draws[2].state.maskedTexture == nullptr);
    f.fighter.update(4, 4.0f / 60, {});
    f.device.draws.clear();
    f.fighter.actor.draw(f.device, Mat4{1}, {}, nullptr, nullptr, &white);
    for (const auto& draw : f.device.draws) {
        REQUIRE(draw.state.maskedTexture == nullptr);
    }
}

TEST_CASE("shipped Garm nodes emit their authored left and right brood projectiles",
          "[combatant-nodes][garm][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/GARM.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/GARM/animations.json");
    test::FakeRenderDevice device;
    test::CombatantFixture fighter;
    fighter.open(device, root, nullptr, {}, 'B');
    REQUIRE(fighter.spawn("GARM", Vec3{0}, 0));
    REQUIRE(fighter.actor.data()->parts().size() == 12);
    REQUIRE(fighter.assets.brokenModels.size() == 12);
    for (const s32 node : {0, 1}) {
        EnemyHit hit;
        hit.damage = 1000;
        hit.node = node;
        const f32 before = fighter.actor.health();
        fighter.actor.hurt(hit);
        REQUIRE(fighter.actor.health() == Approx(before - 250));
        const auto shots = fighter.actor.takeShots();
        REQUIRE(shots.size() == 1);
        REQUIRE(shots.front().damageIndex == node + 1);
    }
}
} // namespace
