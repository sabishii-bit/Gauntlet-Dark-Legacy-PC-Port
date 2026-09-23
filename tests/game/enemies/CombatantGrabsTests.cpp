#include <array>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    std::array<EnemyView, 2> players{};
    Fixture(bool step = false) {
        const auto root = test::scratchDirectory("yeti-grab");
        const auto archive = root / "MONSTERS/YETI";
        std::filesystem::create_directories(root / "critter");
        std::filesystem::create_directories(archive);
        writeTextFile(archive / "body.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeTextFile(archive / "objects.json",
                      R"({"objects":[{"name":"BODY","file":"body.obj"}]})");
        writeFile(archive / "skin.png", test::kTinyPng);
        writeTextFile(archive / "textures.json",
                      R"({"bitmaps":[{"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
        writeTextFile(archive / "animations.json", R"({"trees":[{"name":"YETI",
          "nodes":[{"name":"HAND","object":"BODY","parent":-1,"position":[0,10,0]}],
          "sequences":[{"name":"IDLE","frames":2},{"name":"GRAB","frames":120}]}]})");
        const std::string attack =
            step ? R"({"name":"STEP","type":56,"speed":9.5,
            "anim":"GRAB","priority":10,"cooldown":10})"
                 :
                 R"({"name":"GRAB","type":129,"anim":"GRAB","colnode":"HAND","priority":10,
            "cooldown":10,"frameStart":25,"frameEnd":30,"frameStart2":100,"frameEnd2":-1,
            "damage0":0,"damage1":0})";
        writeTextFile(root / "critter/YETI.json", R"({"descriptors":[{"prefix":"YETI","type":4}],
          "types":[{"moveCount":3,"maxHealth":100,"roamRadius":5}],
          "moves":[{"name":"READY","type":32,"anim":"IDLE"},)" +
                                                      attack + R"(,
            {"name":"DEATH","type":17,"anim":"GRAB","priority":4096}],
          "damages":[{"type":7,"radius":3,"maxDistance":9,"offset":[2,-3,1],
            "damage":100,"minSpeed":1000}]})");
        fighter.open(device, root, nullptr, {}, 'I');
        REQUIRE(fighter.spawn("YETI", Vec3{0}, 0));
        players[0].player = 3;
        players[0].position = {2, 0, 1};
        players[1].player = 1;
        players[1].position = {8, 0, 1};
        fighter.update(2, 0.1f, players); // finish READY
    }
};

TEST_CASE("grab catches one nearest player then releases only at the second damage frame",
          "[game][yeti][combatant-grab]") {
    Fixture f;
    f.fighter.update(2, 24.0f / 30, f.players);
    REQUIRE(f.fighter.actor.moveName() == "GRAB");
    REQUIRE(f.fighter.actor.takeGrabs().empty());
    f.fighter.update(2, 1.0f / 30, f.players);
    auto events = f.fighter.actor.takeGrabs();
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].player == 3);
    REQUIRE(events[0].attachment.has_value());
    REQUIRE(Vec3{(*events[0].attachment)[3]} == Vec3{2, 7, 1});
    REQUIRE(f.fighter.actor.takeBlows().empty()); // holding does not do ordinary melee damage
    f.fighter.actor.freeze(10);
    f.fighter.update(2, 1.0f / 30, f.players);
    REQUIRE(f.fighter.actor.takeGrabs().front().attachment.has_value());
    f.fighter.actor.freeze(0);
    f.fighter.update(2, 74.0f / 30, f.players);
    REQUIRE(f.fighter.actor.takeGrabs().front().attachment.has_value());
    f.fighter.update(2, 2.0f / 30, f.players); // cross frame 100 in a coarse step
    events = f.fighter.actor.takeGrabs();
    REQUIRE(events.size() == 2); // final hand position, then release
    REQUIRE(events.front().attachment.has_value());
    REQUIRE_FALSE(events.back().attachment.has_value());
    REQUIRE(events.back().damage == 100);
    REQUIRE(glm::length(events.back().velocity) == Approx(1000));
    REQUIRE(events.back().velocity.y < 0);
    f.fighter.update(2, 1.0f / 30, f.players);
    REQUIRE(f.fighter.actor.takeGrabs().empty());
}

TEST_CASE("missed grabs do not throw and death frees a held player without damage",
          "[game][yeti][combatant-grab]") {
    Fixture f;
    SECTION("already captured or hidden players cannot be grabbed") {
        f.players[0].captured = true;
        f.players[1].hidden = true;
        f.fighter.update(2, 101.0f / 30, f.players);
        REQUIRE(f.fighter.actor.takeGrabs().empty());
    }
    SECTION("death while holding") {
        f.fighter.update(2, 25.0f / 30, f.players);
        REQUIRE(f.fighter.actor.takeGrabs().size() == 1);
        EnemyHit hit;
        hit.damage = 1000;
        f.fighter.actor.hurt(hit);
        f.fighter.update(2, 1.0f / 30, f.players);
        const auto events = f.fighter.actor.takeGrabs();
        REQUIRE(events.size() == 1);
        REQUIRE_FALSE(events.front().attachment.has_value());
        REQUIRE(events.front().damage == 0);
    }
}

TEST_CASE("destination steps follow the ready target within the authored home radius",
          "[game][yeti][boss-movement]") {
    Fixture f(true);
    f.players[0].position = {30, 0, 0};
    f.players[1].hidden = true;
    f.fighter.update(2, 0.1f, f.players);
    REQUIRE(f.fighter.actor.position().x == Approx(0.95f));
    f.players[0].position = {0, 0, 30};
    f.fighter.update(2, 0.1f, f.players);
    REQUIRE(f.fighter.actor.position().x < 0.95f);
    REQUIRE(f.fighter.actor.position().z > 0);
    f.fighter.update(60, 1.0f, f.players);
    REQUIRE(glm::length(f.fighter.actor.position()) == Approx(5));
}
TEST_CASE("retail Yeti grab uses the animated hand and its frame 100 release",
          "[game][yeti][combatant-grab][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/YETI.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/YETI/animations.json");
    test::FakeRenderDevice device;
    test::CombatantFixture f;
    f.open(device, root, nullptr, {}, 'I');
    REQUIRE(f.spawn("YETI", Vec3{0}, 0));
    const auto& move = f.actor.data()->moves()[32];
    REQUIRE(move.name == "GRAB");
    REQUIRE(move.colnode == "JOINT_13");
    REQUIRE(move.frameStart == 25);
    REQUIRE(move.frameEnd == 30);
    REQUIRE(move.frameStart2 == 100);
    std::array<EnemyView, 1> players{};
    players[0].player = 0;
    players[0].position = {0, 0, 20};
    bool caught = false;
    bool released = false;
    Vec3 first{0};
    bool moved = false;
    for (s32 frame = 0; frame < 6000 && !released; ++frame) {
        if (f.actor.moveName() == "GRAB") {
            // Put the collision cylinder in the animated hand's path. This tests the
            // shipped motion tracks without substituting a static synthetic hand.
            const Mat4 hand = *f.actor.nodeTransform(move.colnode);
            const Mat4 body = *f.actor.rootTransform();
            players[0].position = Vec3{hand[3]} + Vec3{body * Vec4{2, -3, 1, 0}} - Vec3{0, 3, 0};
        }
        f.update(2, 1.0f / 30, players);
        for (const auto& event : f.actor.takeGrabs()) {
            if (event.attachment.has_value()) {
                const Vec3 where{(*event.attachment)[3]};
                if (!caught) {
                    first = where;
                }
                moved = moved || glm::distance(where, first) > 1;
                caught = true;
                REQUIRE(event.player == 0);
            } else {
                REQUIRE(caught);
                REQUIRE(event.damage == 100);
                REQUIRE(glm::length(event.velocity) == Approx(1000));
                released = true;
            }
        }
        f.actor.takeCues();
        f.actor.takeBlows();
        f.actor.takeShots();
    }
    REQUIRE(caught);
    REQUIRE(moved);
    REQUIRE(released);
    // The rocks have impact artwork, but deliberately no expanding splash radius.
    for (const auto index : {2, 3, 9, 10, 13, 14}) {
        REQUIRE(f.actor.data()->damage(index)->maxDistance == 0);
        REQUIRE(f.actor.data()->damage(index)->hitSound == 8);
    }
    REQUIRE(f.actor.data()->sound(8)->flags == 0); // not a floor-aligned SFXX
}
} // namespace
