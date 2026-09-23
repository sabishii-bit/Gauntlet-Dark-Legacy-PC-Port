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
#include "game/enemies/CritterArea.h"
#include "game/enemies/Critters.h"

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

std::filesystem::path areaArchive() {
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
    writeTextFile(root / "critter/DJINN.json", R"({"descriptors":[{"prefix":"DJINN","type":4}],
      "types":[{"moveCount":2,"maxHealth":100,"originOffset":[0,50,0]}],
      "moves":[{"name":"READY","anim":"STEP","type":32},
        {"name":"WHIP","anim":"STEP","type":130,"priority":10,"cooldown":20,
         "frameStart":1,"frameEnd":1,"damage0":0}],
      "damages":[{"type":2,"damage":40,"flags":32,"radius":0.1,"maxDistance":10,
                   "minDot":0.8,"offset":[0,3,0],"sfxIndex":0}],
      "sounds":[{"name":"NULLFX","flags":1,"life":0.5,"offset":[0,2,0]}]})");
    return root;
}

TEST_CASE("invisible areas outlive their attack window and remain rooted while frozen",
          "[game][boss-areas]") {
    const auto root = areaArchive();
    test::FakeRenderDevice device;
    Critters critters;
    EnemyScales scales;
    scales.damage = 2;
    critters.open(device, root, nullptr, scales, 'C');
    const auto id = critters.spawn(kBossCritter, Vec3{0}, 0, "DJINN");
    REQUIRE(id.has_value());
    std::array<EnemyView, 1> players{playerAt({0, 0, 8})};
    critters.update(6, 0.1f, players); // READY ends
    critters.update(6, 0.1f, players); // crosses the entire one-frame WHIP window
    auto blows = critters.takeBlows();
    REQUIRE(blows.size() == 1);
    REQUIRE(blows[0].area);
    REQUIRE(blows[0].damage == 80);
    REQUIRE(blows[0].flags == 32);
    REQUIRE(blows[0].origin == Vec3{0, 5, 0}); // DAMG + SFXX, not the body's origin
    REQUIRE(blows[0].repeatGap == Approx(0.5f));
    REQUIRE(critters.takeCues().empty()); // absence of artwork cannot remove damage
    critters.freeze(*id, 120);
    critters.update(6, 0.1f, players);
    REQUIRE(critters.takeBlows().size() == 1);
    players[0].position = {0, 0, -8};
    critters.update(6, 0.1f, players);
    REQUIRE(critters.takeBlows().empty());
    players[0].position = {0, 0, 8};
    critters.update(24, 0.4f, players);
    REQUIRE(critters.takeBlows().empty());
}

TEST_CASE("Spider Queen and Wraith authored areas use supported root policies",
          "[game][boss-areas][unpacked]") {
    usize total = 0;
    for (const std::string name : {"DRIDER", "WRAITH"}) {
        CritterData data;
        REQUIRE(data.load(test::unpackedOrSkip("critter/" + name + ".json")));
        usize count = 0;
        for (const CritterDamage& damage : data.damages()) {
            if (damage.type != CritterDamage::kAttachedArea) {
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
            Critters critters;
            critters.open(device, root, nullptr, {}, 'D');
            const auto id = critters.spawn(kBossCritter, Vec3{0}, 0, name);
            REQUIRE(id.has_value());
            const std::array<EnemyView, 1> players{playerAt({0, 0, 15})};
            bool areaHit = false;
            for (s32 frame = 0; frame < 1800 && !areaHit; ++frame) {
                critters.update(2, 1.0f / 30.0f, players);
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
            INFO("Last move: " << critters.moveOf(*id));
            REQUIRE(areaHit);
        }
    }
}
} // namespace
