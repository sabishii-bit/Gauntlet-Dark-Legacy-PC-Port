#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/Generators.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;

TEST_CASE("Temple special generators load their own multi-node trees and cycle species",
          "[game][generators][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("ITEMS/LEVELE/animations.json");
    for (const auto* kind : {"ICE", "IMP", "PLA", "ZOM"}) {
        test::unpackedOrSkip(std::string("MONSTERS/") + kind + "/animations.json");
    }
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELE1"));
    ItemArchive items;
    REQUIRE(items.load(root / "ITEMS/LEVELE"));
    Enemies enemies;
    enemies.open(device, root, nullptr, Enemies::kMost, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, nullptr, {}, 1, {}, 5, &items));
    usize expected = 0;
    for (const auto& instance : layout.itemInstances()) {
        if (layout.itemInfos()[static_cast<usize>(instance.info)].type == ItemInfo::kGenerator &&
            shownToParty(instance.minPlayers, 1)) {
            ++expected;
        }
    }
    REQUIRE(expected > 0);
    REQUIRE(generators.count() == expected);
    for (s32 i = 0; i < static_cast<s32>(generators.count()); ++i) {
        CHECK(generators.kindOf(i) == -2);
        CHECK(generators.tierOf(i) == 2);
        CHECK(generators.stateOf(i) == 2);
        CHECK(generators.bodyShown(i));
    }
    const std::array views{EnemyView{.position = generators.positionOf(0)}};
    generators.update(2, enemies, views);
    REQUIRE(enemies.count() > 0);
    generators.draw(device, Mat4{1}, {});
    REQUIRE_FALSE(device.draws.empty());
    generators.strike(0, 100000, 0);
    CHECK(generators.stateOf(0) == 0);
    CHECK(generators.bodyShown(0)); // broken ruin remains
}

/** A field with one grunt generator of strength two at the origin facing +z, one of strength
 * three at x 60 for a party of two, and a rats' one at x 120. */
std::filesystem::path sampleLevel(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "GROUND", "position": [0, 0, 0], "next": -1, "child": -1}],
  "animations": [], "particles": [], "locators": [],
  "itemInfos": [
    {"type": 3, "name": "GRU", "radius": 2, "height": 5, "collisionType": 1, "hitPoints": 10,
     "activeType": 5, "activeOff": -30, "activeOn": 1},
    {"type": 3, "name": "RAT", "radius": 2, "height": 5, "collisionType": 1, "hitPoints": 10},
    {"type": 3, "name": "BOSSGEN", "radius": 2, "height": 5, "hitPoints": 10}
  ],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "position": [0, 0, 0], "rotation": [0, 0, 0],
     "params": [2, 0, 7, 0, 5, 0, 20, 0, 0, 0, 0, 0]},
    {"info": 0, "minPlayers": 2, "position": [60, 0, 0], "rotation": [0, 0, 0],
     "params": [3, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 1, "minPlayers": 1, "position": [120, 0, 0], "rotation": [0, 0, 0],
     "params": [1, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 2, "minPlayers": 1, "position": [180, 0, 0], "rotation": [0, 0, 0],
     "params": [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}
  ]
})");
    return dir;
}

TEST_CASE("boss generators use the stage record and breed after the birth delay",
          "[spider][unpacked]") {
    const auto root = test::unpackedOrSkip("MONSTERS/SPI/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 17, {}, 3);
    Generators generators;
    ItemArchive noArt;
    ItemInfo info;
    info.type = ItemInfo::kGenerator;
    info.name = "BOSSGEN";
    info.hitPoints = 10;
    info.height = 5;
    info.radius = 2;
    info.armor = 0;
    REQUIRE(generators.placeBoss(device, info, noArt, enemies, 9, Mat4{1}, nullptr));
    REQUIRE(generators.count() == 1);
    REQUIRE(generators.kindOf(0) == 9);
    REQUIRE(generators.healthOf(0) == 10);
    REQUIRE(generators.tierOf(0) == 1);
    REQUIRE(generators.mostOf(0) == 10);
    REQUIRE(generators.intervalOf(0) == 5);
    REQUIRE(generators.countdownOf(0) == 40);
    REQUIRE_FALSE(generators.bodyShown(0));
    const std::vector<EnemyView> party{{0, {0, 0, 20}, 1, 6}};
    generators.update(40, enemies, party);
    REQUIRE(generators.bredOf(0) == 0);
    generators.update(2, enemies, party);
    REQUIRE(generators.bredOf(0) == 1);
    const auto event = generators.strike(0, 10, 0);
    REQUIRE(event.has_value());
    REQUIRE(event->destroyed);
    generators.update(600, enemies, party);
    REQUIRE(generators.bredOf(0) == 1);
}

TEST_CASE("the fields place forty-seven generators for a party of one, of grunts and rats",
          "[game][enemies][unpacked]") {
    const std::filesystem::path level =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path();
    const std::filesystem::path root = level.parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/GRU/animations.json");
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(level));
    WorldCollision collision;
    REQUIRE(collision.load(level, layout));
    Enemies enemies;
    enemies.open(device, root, &collision, 13, EnemyScales{}, 3);
    Generators generators;
    REQUIRE(generators.bind(device, layout, enemies, &collision, GeneratorScales{}, 1));
    REQUIRE(generators.count() == 47);
    REQUIRE(enemies.kindLoaded(kGruntKind));
    REQUIRE(enemies.kindLoaded(kRatKind));
    // With the fields' roster the same records breed zombies and maggots instead.
    test::unpackedOrSkip("MONSTERS/ZOM/animations.json");
    const std::vector<LevelEnemy> fields{{13, kMediumClass, {}}, {12, kSmallClass, {}}};
    Enemies bred;
    bred.open(device, root, &collision, 13, EnemyScales{}, 3);
    Generators graves;
    REQUIRE(graves.bind(device, layout, bred, &collision, GeneratorScales{}, 1, fields));
    REQUIRE(graves.count() == 47);
    REQUIRE(bred.kindLoaded(13));
    REQUIRE(bred.kindLoaded(12));
    REQUIRE_FALSE(bred.kindLoaded(kGruntKind));
    REQUIRE(graves.kindOf(0) == 13);
    REQUIRE(graves.bodyShown(0));
    Generators wider;
    REQUIRE(wider.bind(device, layout, enemies, &collision, GeneratorScales{}, 4));
    REQUIRE(wider.count() == 117);
    // Each stands whole, boxed as its record says, and is found by a sweep.
    for (usize g = 0; g < generators.count(); ++g) {
        const auto id = static_cast<s32>(g);
        REQUIRE(generators.standing(id));
        REQUIRE(generators.stateOf(id) == 3);
        REQUIRE(generators.tierOf(id) >= 1);
        REQUIRE(generators.tierOf(id) <= 3);
        REQUIRE(generators.mostOf(id) > 0);
        REQUIRE(generators.intervalOf(id) > 0);
    }
    REQUIRE(generators.obstacles().size() == 47);
    const Vec3 at = generators.positionOf(0);
    REQUIRE((generators.struckBy(at + Vec3{-8.0f, 2.0f, 0.0f}, at + Vec3{8.0f, 2.0f, 0.0f}, 0.5f) ==
             0));
    REQUIRE_FALSE(generators.within(at + Vec3{0.0f, 2.0f, 0.0f}, 1.0f).empty());
    REQUIRE_FALSE(
        generators.struckBy(at + Vec3{-8.0f, 40.0f, 0.0f}, at + Vec3{8.0f, 40.0f, 0.0f}, 0.5f)
            .has_value());
}

TEST_CASE("a generator breeds grunts for a party near it up to its count, and crumbles when struck",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root = test::unpackedOrSkip("MONSTERS/GRU/animations.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    test::FakeRenderDevice device;
    WorldLayout layout;
    REQUIRE(layout.load(sampleLevel("generators-field")));
    Enemies enemies;
    EnemyScales scales;
    scales.health = 0.75f;
    enemies.open(device, root, nullptr, 13, scales, 3);
    Generators generators;
    GeneratorScales generatorScales;
    generatorScales.health = 0.75f;
    generatorScales.most = 0.75f;
    generatorScales.rate = 1.5f;
    // The strength-three one is for a party of two; the boss generator is no kind.
    REQUIRE(generators.bind(device, layout, enemies, nullptr, generatorScales, 1));
    REQUIRE(generators.count() == 2);
    REQUIRE(enemies.kindLoaded(kGruntKind));
    REQUIRE(enemies.kindLoaded(kRatKind));
    // Strength two: two tiers of health at the level's three quarters, three out at once
    // (five at three quarters, made whole), a countdown of thirty (twenty at one and a half).
    const s32 chosen = 0;
    REQUIRE(generators.kindOf(chosen) == kGruntKind);
    REQUIRE(generators.tierOf(chosen) == 2);
    REQUIRE(generators.mostOf(chosen) == 3);
    REQUIRE(generators.intervalOf(chosen) == 30);
    REQUIRE(generators.healthOf(chosen) == Approx(15.0f));
    REQUIRE(generators.stateOf(chosen) == 3);
    REQUIRE(generators.standing(chosen));
    REQUIRE(generators.bodyShown(chosen));
    // The rats' one takes its defaults: ten at once, five between, at the record's scales.
    REQUIRE(generators.kindOf(1) == kRatKind);
    REQUIRE(generators.mostOf(1) == 7);
    REQUIRE(generators.intervalOf(1) == 7);
    // Nobody near: nothing is bred. A player near it: one at once, and the countdown set
    // going, stretched a little more each birth.
    const std::vector<EnemyView> nobody;
    generators.update(kTicks, enemies, nobody);
    REQUIRE(enemies.count() == 0);
    EnemyView near;
    near.player = 0;
    near.position = Vec3{0.0f, 0.0f, 30.0f};
    const std::vector<EnemyView> party{near};
    generators.update(kTicks, enemies, party);
    REQUIRE(generators.bredOf(chosen) == 1);
    REQUIRE(generators.bredOf(1) == 0);             // too far off
    REQUIRE(generators.countdownOf(chosen) == 180); // six ticks a unit of interval
    std::vector<s32> mine;
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (enemies.alive(id) && enemies.generatorOf(id) == chosen) {
            mine.push_back(id);
        }
    }
    REQUIRE(mine.size() == 1);
    REQUIRE(enemies.kindOf(mine[0]) == kGruntKind);
    REQUIRE(enemies.tierOf(mine[0]) == 2);
    REQUIRE(enemies.algorithmOf(mine[0]) == 7);
    REQUIRE(enemies.positionOf(mine[0]).z > 4.0f); // ahead of it
    REQUIRE(glm::length(enemies.positionOf(mine[0])) == Approx(6.5f).margin(0.01f));
    // It breeds up to its count and no further while they live.
    s32 bred = 1;
    for (s32 i = 0; i < 600 && bred < 3; ++i) {
        generators.update(kTicks, enemies, party);
        enemies.update(kTicks, kStep, party);
        bred = generators.bredOf(chosen);
    }
    REQUIRE(bred == 3);
    REQUIRE(generators.countdownOf(chosen) == 240); // stretched by a third
    for (s32 i = 0; i < 400; ++i) {
        generators.update(kTicks, enemies, party);
        enemies.update(kTicks, kStep, party);
    }
    REQUIRE(generators.bredOf(chosen) == 3);
    REQUIRE(enemies.count() == 3);
    // One killed makes room for another.
    EnemyHit slay;
    slay.damage = 100.0f;
    slay.player = 0;
    enemies.hurt(mine[0], slay);
    for (s32 i = 0; i < 400 && generators.bredOf(chosen) < 4; ++i) {
        generators.update(kTicks, enemies, party);
        enemies.update(kTicks, kStep, party);
    }
    REQUIRE(generators.bredOf(chosen) == 4);
    // Struck: three of armour come off each blow, a point always getting through. Its
    // health being two of its record's, the first blow crumbles it a state, as the original
    // has it; at one of its record's it crumbles another, and then it is gone, breeding no
    // more.
    auto event = generators.strike(chosen, 2.0f, 0);
    REQUIRE(generators.healthOf(chosen) == Approx(14.0f));
    REQUIRE(event.has_value());
    REQUIRE(event->state == 2);
    REQUIRE_FALSE(event->destroyed);
    REQUIRE(event->kind == kGruntKind);
    REQUIRE(event->generator == chosen);
    REQUIRE(generators.stateOf(chosen) == 2);
    event = generators.strike(chosen, 4.0f, 0);
    REQUIRE(event);
    REQUIRE_FALSE(event->stateChanged);
    event = generators.strike(chosen, 8.0f, 0);
    REQUIRE(event);
    REQUIRE_FALSE(event->stateChanged);
    REQUIRE(generators.healthOf(chosen) == Approx(8.0f));
    event = generators.strike(chosen, 3.0f, 0);
    REQUIRE(event.has_value());
    REQUIRE(event->state == 1);
    event = generators.strike(chosen, 50.0f, -1);
    REQUIRE(event.has_value());
    REQUIRE(event->destroyed);
    REQUIRE_FALSE(generators.standing(chosen));
    REQUIRE(generators.obstacles().size() == 1);
    REQUIRE_FALSE(generators.strike(chosen, 50.0f, 0).has_value());
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (enemies.alive(id)) {
            enemies.hurt(id, slay);
        }
    }
    for (s32 i = 0; i < 300; ++i) {
        generators.update(kTicks, enemies, party);
        enemies.update(kTicks, kStep, party);
    }
    REQUIRE(generators.bredOf(chosen) == 4);
    REQUIRE(enemies.count() == 0);
}

TEST_CASE("a generator's record gives its strength, way, count and interval, or their defaults",
          "[game][enemies]") {
    ItemInstance instance;
    instance.params = {2, 0, 7, 0, 5, 0, 20, 0, 0, 0, 0, 0};
    REQUIRE(Generators::paramOf(instance, 0) == 2);
    REQUIRE(Generators::paramOf(instance, 1) == 7);
    REQUIRE(Generators::paramOf(instance, 2) == 5);
    REQUIRE(Generators::paramOf(instance, 3) == 20);
    REQUIRE(Generators::paramOf(instance, 6) == 0);
    instance.params = {1, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE(Generators::paramOf(instance, 1) == -1);
    REQUIRE(Generators::kDefaultMost[0] == 10);
    REQUIRE(Generators::kDefaultInterval[2] == 15);
}

} // namespace
