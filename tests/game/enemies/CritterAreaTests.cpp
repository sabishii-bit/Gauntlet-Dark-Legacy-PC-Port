#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Combatant.h"
#include "game/enemies/CombatantFixture.h"
#include "game/enemies/CritterArea.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

EnemyView playerAt(const Vec3& position) {
    EnemyView view;
    view.player = 2;
    view.position = position;
    view.radius = 1;
    view.height = 6;
    return view;
}

TEST_CASE("attached damage sectors use horizontal reach height and a directional cone",
          "[game][boss-areas]") {
    CritterArea area;
    area.radius = 10;
    area.minDot = 0.8f;
    area.secondsLeft = 1;
    REQUIRE(area.touches(Mat4{1}, playerAt({0, 0, 11})));
    REQUIRE_FALSE(area.touches(Mat4{1}, playerAt({0, 0, 11.01f})));
    REQUIRE_FALSE(area.touches(Mat4{1}, playerAt({0, 0, -5})));
    REQUIRE(area.touches(Mat4{1}, playerAt({0, 10, 5})));
    REQUIRE_FALSE(area.touches(Mat4{1}, playerAt({0, 10.01f, 5})));
    // Near the origin the cone is relaxed to 85% of its authored dot threshold.
    const Vec3 direction{std::sqrt(1 - 0.7f * 0.7f), 0, 0.7f};
    REQUIRE(area.touches(Mat4{1}, playerAt(direction * 2.0f)));
    REQUIRE_FALSE(area.touches(Mat4{1}, playerAt(direction * 8.0f)));
    area.minDot = -1;
    REQUIRE(area.touches(Mat4{1}, playerAt({0, 0, -5})));
    auto hidden = playerAt({0, 0, 5});
    hidden.hidden = true;
    REQUIRE_FALSE(area.touches(Mat4{1}, hidden));
    area.secondsLeft = 0;
    REQUIRE_FALSE(area.touches(Mat4{1}, playerAt({0, 0, 5})));
}

TEST_CASE("attached areas rotate their cone independently of their translated origin",
          "[game][boss-areas]") {
    const f32 quarter = std::numbers::pi_v<f32> / 2;
    CritterArea area;
    area.radius = 10;
    area.minDot = 0.8f;
    area.secondsLeft = 1;
    area.local = CritterArea::placement(Mat4{1}, {0, 3, 2}, {0, quarter});
    REQUIRE(Vec3{area.local[3]} == Vec3{0, 3, 2});
    REQUIRE(area.touches(Mat4{1}, playerAt({5, 0, 2})));
    REQUIRE_FALSE(area.touches(Mat4{1}, playerAt({-5, 0, 2})));
    const Mat4 parent =
        glm::rotate(glm::translate(Mat4{1}, Vec3{20, 0, 30}), quarter, Vec3{0, 1, 0});
    REQUIRE(area.touches(parent, playerAt({22, 0, 25})));
    REQUIRE_FALSE(area.touches(parent, playerAt({22, 0, 35})));
}

TEST_CASE("area immunity is capped by remaining life with an explicit half-second flag",
          "[game][boss-areas]") {
    CritterArea area;
    area.damage = 50;
    area.secondsLeft = 3;
    REQUIRE(area.hitGap() == 1);
    area.secondsLeft = 0.1f;
    REQUIRE(area.hitGap() == Approx(0.1f));
    area.flags = 0x800;
    REQUIRE(area.hitGap() == 0.5f);
    area.flags = 0;
    area.damage = 2;
    REQUIRE(area.hitGap() == 0);
}

TEST_CASE("expanding damage starts near the source then fades before the effect ends",
          "[game][boss-areas][chimera]") {
    CritterArea area;
    area.expanding = true;
    area.lifetime = area.secondsLeft = 1;
    area.radius = 95;
    area.damage = 75;
    area.minDot = 0;
    REQUIRE(area.currentRadius() == Approx(31.35f));
    REQUIRE(area.currentDamage() == Approx(75.375f));
    REQUIRE(area.hitGap() == Approx(1.066667f));
    REQUIRE(area.touches(Mat4{1}, playerAt({0, 0, 30})));
    REQUIRE_FALSE(area.touches(Mat4{1}, playerAt({0, 0, 60})));
    REQUIRE_FALSE(area.touches(Mat4{1}, playerAt({0, 0, -30})));

    area.secondsLeft = 0.5f;
    REQUIRE(area.currentRadius() == Approx(78.85f));
    REQUIRE(area.currentDamage() == Approx(19.125f));
    REQUIRE(area.hitGap() == Approx(0.566667f));
    REQUIRE(area.touches(Mat4{1}, playerAt({0, 0, 60})));
    REQUIRE_FALSE(area.touches(Mat4{1}, playerAt({0, 0, -60})));
    area.secondsLeft = 0.34f;
    REQUIRE(area.currentDamage() < 2);
    REQUIRE(area.hitGap() == 0);
    area.flags = 0x800;
    REQUIRE(area.hitGap() == 0.5f);

    area.secondsLeft = 0.33f;
    REQUIRE(area.currentRadius() == 0);
    REQUIRE(area.currentDamage() == 0);
    REQUIRE_FALSE(area.touches(Mat4{1}, playerAt({0, 0, 30})));
    area.lifetime = area.secondsLeft = 1.0f / 60.0f;
    REQUIRE(area.currentRadius() == Approx(31.35f));
    area.secondsLeft = 0;
    REQUIRE(area.currentRadius() == 0);
    REQUIRE(area.currentDamage() == 0);
}

std::filesystem::path areaArchive(bool expanding = false) {
    const auto root = test::scratchDirectory("boss-area-attacks");
    const auto archive = root / "MONSTERS/DJINN";
    std::filesystem::create_directories(root / "critter");
    std::filesystem::create_directories(archive / "models");
    std::filesystem::create_directories(archive / "textures");
    writeTextFile(archive / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"models/body.obj","meshTriangles":1}]})");
    writeFile(archive / "textures/skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2,"flags":0}]})");
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"DJINN",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"STEP","frames":3}]}]})");
    const std::string type = expanding ? "3" : "2";
    const std::string flags = expanding ? "0" : "1";
    writeTextFile(root / "critter/DJINN.json", R"({"descriptors":[{"prefix":"DJINN","type":4}],
      "types":[{"moveCount":2,"maxHealth":100,"originOffset":[0,50,0]}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
        {"name":"WHIP","anim":"STEP","type":130,"priority":10,"cooldown":20,
         "frameStart":1,"frameEnd":1,"damage0":0}],
      "damages":[{"type":)" + type + R"(,"damage":40,"flags":32,"radius":0.1,"maxDistance":10,
                   "minDot":0.8,"offset":[0,3,0],"sfxIndex":0}],
      "sounds":[{"name":"NULLFX","flags":)" + flags +
                                                   R"(,"life":0.5,"offset":[0,2,0]}]})");
    return root;
}

TEST_CASE("invisible areas outlive their attack window and remain rooted while frozen",
          "[game][boss-areas]") {
    const auto root = areaArchive();
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    Combatant& critters = fixture.actor;
    EnemyScales scales;
    scales.damage = 2;
    fixture.open(device, root, nullptr, scales, 'C');
    REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
    REQUIRE(critters.present());
    std::array<EnemyView, 1> players{playerAt({0, 0, 8})};
    fixture.update(6, 0.1f, players); // READY ends
    fixture.update(6, 0.1f, players); // crosses the entire one-frame WHIP window
    auto blows = critters.takeBlows();
    REQUIRE(blows.size() == 1);
    REQUIRE(blows[0].area);
    REQUIRE(blows[0].damage == 80);
    REQUIRE(blows[0].flags == 32);
    REQUIRE(blows[0].origin == Vec3{0, 5, 0}); // DAMG + SFXX, not the body's origin
    REQUIRE(blows[0].repeatGap == Approx(0.5f));
    REQUIRE(critters.takeCues().empty()); // absence of artwork cannot remove damage
    critters.freeze(120);
    fixture.update(6, 0.1f, players);
    REQUIRE(critters.takeBlows().size() == 1);
    players[0].position = {0, 0, -8};
    fixture.update(6, 0.1f, players);
    REQUIRE(critters.takeBlows().empty());
    players[0].position = {0, 0, 8};
    fixture.update(24, 0.4f, players);
    REQUIRE(critters.takeBlows().empty());
}

TEST_CASE("type three damage waits for expansion and survives the move while frozen",
          "[game][boss-areas][chimera]") {
    const auto root = areaArchive(true);
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'A');
    REQUIRE(fixture.spawn("DJINN", Vec3{0}, 0));
    auto& actor = fixture.actor;
    const std::array<EnemyView, 1> players{playerAt({0, 0, 8})};
    fixture.update(6, 0.1f, players);
    fixture.update(6, 0.1f, players);
    REQUIRE(actor.moveName() == "WHIP");
    REQUIRE(actor.takeBlows().empty()); // radius 3.3, not the full authored 10
    actor.freeze(120);
    fixture.update(6, 0.1f, players);
    REQUIRE(actor.takeBlows().empty()); // radius 5.3
    fixture.update(6, 0.1f, players);
    const auto hits = actor.takeBlows();
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].area);
    REQUIRE(hits[0].damage == Approx(16.2f));
    REQUIRE(hits[0].repeatGap == Approx(0.366667f));
    REQUIRE(hits[0].origin == Vec3{0, 5, 0});
    fixture.update(12, 0.2f, players);
    REQUIRE(actor.takeBlows().empty()); // final visual tail is harmless
}

TEST_CASE("Spider Queen and Wraith authored areas use supported root policies",
          "[game][boss-areas][unpacked]") {
    usize total = 0;
    for (const std::string name : {"DRIDER", "WRAITH"}) {
        CritterData data;
        REQUIRE(data.load(test::unpackedOrSkip("critter/" + name + ".json")));
        usize count = 0;
        for (const AttackDefinition& damage : data.damages()) {
            if (damage.type != AttackDefinition::kAttachedArea) {
                continue;
            }
            ++count;
            const auto* sound = data.sound(damage.sound);
            REQUIRE(sound != nullptr);
            REQUIRE((sound->flags & 1U) != 0);
            REQUIRE((sound->flags & ~5U) == 0);
            REQUIRE(sound->link == -1);
            REQUIRE(damage.behaviorFlags == 0);
            REQUIRE(damage.speed == 0);
            REQUIRE(damage.morph == -1);
            REQUIRE(damage.maxDistance > 0);
            REQUIRE(damage.damage > 0);
        }
        REQUIRE(count == (name == "DRIDER" ? 8 : 3));
        total += count;
    }
    REQUIRE(total == 11);
}

TEST_CASE("Spider Queen and Wraith encounters emit damaging areas from their authored moves",
          "[game][boss-areas][unpacked]") {
    for (const std::string name : {"DRIDER", "WRAITH"}) {
        DYNAMIC_SECTION(name) {
            const auto root =
                test::unpackedOrSkip("critter/" + name + ".json").parent_path().parent_path();
            test::unpackedOrSkip("MONSTERS/" + name + "/animations.json");
            test::FakeRenderDevice device;
            test::CombatantFixture fixture;
            Combatant& critters = fixture.actor;
            fixture.open(device, root, nullptr, {}, 'D');
            REQUIRE(fixture.spawn(name, Vec3{0}, 0));
            REQUIRE(critters.present());
            const std::array<EnemyView, 1> players{playerAt({0, 0, 15})};
            bool areaHit = false;
            for (s32 frame = 0; frame < 1800 && !areaHit; ++frame) {
                fixture.update(2, 1.0f / 30.0f, players);
                for (const auto& blow : critters.takeBlows()) {
                    if (blow.area) {
                        REQUIRE(blow.player == 2);
                        REQUIRE(blow.damage > 0);
                        REQUIRE(blow.repeatGap > 0);
                        areaHit = true;
                    }
                }
                critters.takeCues();
                critters.takeShots();
            }
            INFO("Last move: " << critters.moveName());
            REQUIRE(areaHit);
        }
    }
}

TEST_CASE("Chimera SUPER expands towards the player and attaches fire to the body",
          "[game][boss-areas][chimera][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/CHIMERA.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/CHIMERA/animations.json");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    Combatant& actor = fixture.actor;
    fixture.open(device, root, nullptr, {}, 'A');
    const f32 quarter = std::numbers::pi_v<f32> / 2;
    REQUIRE(fixture.spawn("CHIMERA", Vec3{0}, quarter));
    const std::array<EnemyView, 1> players{playerAt({60, 0, 0})};
    bool hit = false;
    bool fire = false;
    for (s32 frame = 0; frame < 1800 && !hit; ++frame) {
        fixture.update(2, 1.0f / 30.0f, players);
        for (const auto& cue : actor.takeCues()) {
            if (cue.tree == "SFIRE2") {
                REQUIRE(cue.follows);
                REQUIRE(cue.rootAttachment);
                REQUIRE(cue.nodeOffset == Vec3{0, 8, 20});
                const auto parent = actor.rootTransform();
                REQUIRE(parent.has_value());
                const Vec3 expected = Vec3{*parent * Vec4{0, 8, 20, 1}};
                REQUIRE(glm::length(cue.position - expected) < 0.01f);
                REQUIRE(cue.position.x - (*parent)[3].x ==
                        Approx(20 * actor.scale()).margin(0.01f));
                fire = true;
            }
        }
        for (const auto& blow : actor.takeBlows()) {
            REQUIRE(blow.area);
            REQUIRE(blow.damage > 0);
            // At sixty units the growing sector reaches the player only after
            // its initial high-damage phase; the old instant full-radius hit did 75.
            REQUIRE(blow.damage < 75);
            REQUIRE(blow.repeatGap > 0);
            hit = true;
        }
        actor.takeShots();
    }
    INFO("Last move: " << actor.moveName());
    REQUIRE(fire);
    REQUIRE(hit);
}
} // namespace
