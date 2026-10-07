#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/app/Scenario.h"
#include "game/enemies/CritterPatrol.h"
#include "game/enemies/Critters.h"
#include "game/enemies/EnemyMind.h"
#include "game/world/LevelWorld.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;
constexpr f32 kPi = std::numbers::pi_v<f32>;

WorldLocator sentry(const Vec3& position, u32 next) {
    WorldLocator locator;
    locator.kind = LocatorKind::Sentry;
    locator.position = position;
    locator.next = next;
    return locator;
}

/** Two lookouts naming each other along z, and a third naming nothing. */
LookoutRoute twoPosts() {
    return LookoutRoute::of(std::vector<WorldLocator>{
        sentry(Vec3{0, 0, 8}, 1), sentry(Vec3{0, 0, -8}, 0), sentry(Vec3{40, 0, 0}, 2)});
}

/** A general of one costume whose walk turns at a right angle a second. */
std::filesystem::path generalAssets() {
    const auto root = test::scratchDirectory("critter-patrol");
    const auto archive = root / "MONSTERS/GENERAL/LEVELG";
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
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"GENERAL1",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"STEP","frames":3}]}]})");
    writeTextFile(root / "critter/GENERAL.json", R"({
      "descriptors":[{"prefix":"GENERAL","name":"GENERAL","type":8}],
      "types":[{"suffix":"1","moveCount":5,"maxHealth":200,"radius":1,"wallRadius":1,
                "expValue":100,"target":{"maxDistance":30}}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"priority":256,"interrupt":40},
               {"name":"CHARGE","anim":"STEP","type":134,"priority":784,"speed":15,
                "target":{"minDistance":10}},
               {"name":"STEPTO","anim":"STEP","type":56,"priority":272,"speed":5},
               {"name":"WALK","anim":"STEP","type":52,"priority":272,"speed":5,
                "turnRate":1.5707964,"target":{"minDistance":5}},
               {"name":"DEATH","anim":"STEP","type":17,"priority":4095}]})");
    return root;
}

EnemyView playerAt(const Vec3& position) {
    EnemyView view;
    view.player = 0;
    view.position = position;
    view.radius = 1.0f;
    view.height = 6.0f;
    return view;
}

TEST_CASE("a round of the lookouts starts at the nearest chained one within ten and goes on "
          "past each reached to the one it names",
          "[game][enemies][critter-patrol]") {
    const LookoutRoute route = twoPosts();
    CritterPatrol patrol;
    // Nothing within ten: no round; nor from a lookout naming nothing but itself.
    patrol.start(&route, Vec3{0, 0, 30});
    REQUIRE_FALSE(patrol.active());
    REQUIRE_FALSE(patrol.aim(Vec3{0, 0, 30}).has_value());
    patrol.start(&route, Vec3{41, 0, 0});
    REQUIRE_FALSE(patrol.active());
    patrol.start(nullptr, Vec3{0, 0, 7});
    REQUIRE_FALSE(patrol.active());
    // The nearest of the chain: the first post, by three-dimensional distance.
    patrol.start(&route, Vec3{0, 0, 2});
    REQUIRE(patrol.active());
    REQUIRE(patrol.lookout() == 0);
    REQUIRE(patrol.aim(Vec3{0, 0, 2}) == Vec3{0, 0, 8});
    // Within a unit of it, the aim moves on to the second, and back again.
    REQUIRE(patrol.aim(Vec3{0.5f, 0, 8.5f}) == Vec3{0, 0, -8});
    REQUIRE(patrol.lookout() == 1);
    REQUIRE(patrol.aim(Vec3{0, 0, -7.5f}) == Vec3{0, 0, 8});
    REQUIRE(patrol.lookout() == 0);
    // Exactly a unit off still counts as short of it.
    REQUIRE(patrol.aim(Vec3{0, 0, 7}) == Vec3{0, 0, 8});
    patrol.end();
    REQUIRE_FALSE(patrol.active());
    REQUIRE_FALSE(patrol.aim(Vec3{0, 0, 8}).has_value());
    // A chain that ends is walked to its end and no further.
    const LookoutRoute open = LookoutRoute::of(
        std::vector<WorldLocator>{sentry(Vec3{0, 0, 5}, 1), sentry(Vec3{0, 0, 15}, 1)});
    patrol.start(&open, Vec3{0, 0, 0});
    REQUIRE(patrol.lookout() == 0);
    REQUIRE(patrol.aim(Vec3{0, 0, 5}) == Vec3{0, 0, 15});
    REQUIRE_FALSE(patrol.aim(Vec3{0, 0, 15}).has_value());
    REQUIRE_FALSE(patrol.active());
}

TEST_CASE("a general walks its round of the lookouts, turning to each, and gives it up for a "
          "player within its sight or a blow",
          "[game][enemies][critter-patrol]") {
    const auto root = generalAssets();
    test::FakeRenderDevice device;
    EnemyScales scales;
    scales.sight = 2.0f;
    Critters critters;
    critters.open(device, root, nullptr, scales, 'G');
    critters.setLookouts(twoPosts());
    // Placed facing +x midway between the posts, with a sight parameter of five: the first
    // post, as near as the other, is taken first.
    const auto id = critters.spawn(CombatantKind::General, Vec3{0, 0, 0}, kPi / 2, "", 5.0f);
    REQUIRE(id.has_value());
    REQUIRE(critters.patrolling(*id));
    REQUIRE(critters.lookoutOf(*id) == 0);
    // Nobody about: it walks (the first step move of the walk family that goes anywhere,
    // not the one that wants a player), turning to the post at its walk's rate.
    const std::vector<EnemyView> nobody;
    for (s32 i = 0; i < 4; ++i) {
        critters.update(kTicks, kStep, nobody);
    }
    REQUIRE(critters.moveOf(*id) == "WALK");
    REQUIRE(critters.yawOf(*id) < kPi / 2);
    REQUIRE(critters.yawOf(*id) > 0.0f);
    for (s32 i = 0; i < 32; ++i) {
        critters.update(kTicks, kStep, nobody);
    }
    REQUIRE(std::abs(critters.yawOf(*id)) < 0.35f); // near enough +z, the post
    REQUIRE(critters.positionOf(*id).z > 1.0f);
    REQUIRE(critters.positionOf(*id).x > 1.0f); // having curved round towards it
    // It reaches the first post and goes on to the second, then back: a round.
    bool turnedBack = false;
    for (s32 i = 0; i < 900 && !turnedBack; ++i) {
        critters.update(kTicks, kStep, nobody);
        turnedBack = critters.lookoutOf(*id) == 1;
    }
    REQUIRE(turnedBack);
    REQUIRE(critters.positionOf(*id).z > 6.0f);
    bool roundTrip = false;
    for (s32 i = 0; i < 1800 && !roundTrip; ++i) {
        critters.update(kTicks, kStep, nobody);
        roundTrip = critters.lookoutOf(*id) == 0;
    }
    REQUIRE(roundTrip);
    REQUIRE(critters.positionOf(*id).z < -6.0f);
    REQUIRE(critters.patrolling(*id));
    // A player within the type's sight but past the placement's (five at a scale of two:
    // a score of ten, which is the distance squarely ahead and twice it to the side) is
    // not taken; one within it ends the round for good, and the general goes after them.
    const Vec3 here = critters.positionOf(*id);
    const f32 facing = critters.yawOf(*id);
    const Vec3 ahead{std::sin(facing), 0.0f, std::cos(facing)};
    const Vec3 aside{ahead.z, 0.0f, -ahead.x};
    critters.update(kTicks, kStep, std::vector<EnemyView>{playerAt(here + aside * 6.0f)});
    REQUIRE(critters.targetOf(*id) == -1);
    REQUIRE(critters.patrolling(*id));
    critters.update(kTicks, kStep, std::vector<EnemyView>{playerAt(here + ahead * 12.0f)});
    REQUIRE(critters.targetOf(*id) == -1);
    REQUIRE(critters.patrolling(*id));
    critters.update(kTicks, kStep, std::vector<EnemyView>{playerAt(here + ahead * 8.0f)});
    REQUIRE(critters.targetOf(*id) == 0);
    REQUIRE_FALSE(critters.patrolling(*id));
    critters.update(kTicks, kStep, nobody);
    REQUIRE_FALSE(critters.patrolling(*id));
    // Hurt, another gives up its round at once.
    critters.setLookouts(twoPosts());
    const auto other = critters.spawn(CombatantKind::General, Vec3{0, 0, -4}, 0.0f, "", 5.0f);
    REQUIRE(other.has_value());
    REQUIRE(critters.patrolling(*other));
    EnemyHit hit;
    hit.damage = 5.0f;
    hit.player = 0;
    critters.hurt(*other, hit);
    REQUIRE_FALSE(critters.patrolling(*other));
    // Without lookouts, or with none within ten, there is no round to walk.
    Critters alone;
    alone.open(device, root, nullptr, scales, 'G');
    const auto lone = alone.spawn(CombatantKind::General, Vec3{0, 0, 2}, 0.0f, "", 5.0f);
    REQUIRE(lone.has_value());
    REQUIRE_FALSE(alone.patrolling(*lone));
    for (s32 i = 0; i < 30; ++i) {
        alone.update(kTicks, kStep, nobody);
    }
    REQUIRE(alone.moveOf(*lone) == "READY");
    REQUIRE(alone.positionOf(*lone) == Vec3{0, 0, 2});
}

TEST_CASE("the castle's generals pace between the two posts each is placed by",
          "[game][enemies][critter-patrol][assets]") {
    const auto root = test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/GENERAL/LEVELA/ANIM.PS2");
    test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELA1"));
    const LookoutRoute route = LookoutRoute::of(layout.locators());
    REQUIRE(route.points.size() >= 8);
    test::FakeRenderDevice device;
    Critters critters;
    critters.open(device, root, nullptr, EnemyScales{}, 'A');
    critters.setLookouts(route);
    // The general placed for one player by the posts at (-30.8, 0, 37.1) and (-30.9, 0, 30.6).
    const Vec3 placed{-30.9f, 0.0f, 33.8f};
    const auto id = critters.spawn(CombatantKind::General, placed, -kPi / 2, "", 10.0f);
    REQUIRE(id.has_value());
    REQUIRE(critters.patrolling(*id));
    const s32 first = critters.lookoutOf(*id);
    REQUIRE(first >= 0);
    REQUIRE(route.next[static_cast<usize>(first)] != first);
    const Vec3 post = route.points[static_cast<usize>(first)];
    REQUIRE(glm::distance(post, placed) < CritterPatrol::kStartReach);
    // Its entrance over, it walks to the post at its WALK's five a second and on to the
    // other, sounding its steps as it goes.
    const std::vector<EnemyView> nobody;
    s32 reached = -1;
    usize steps = 0;
    for (s32 i = 0; i < 1500 && reached < 0; ++i) {
        critters.update(kTicks, kStep, nobody);
        for (const CombatCue& cue : critters.takeCues()) {
            steps += cue.sound == "S_GENASTEP1" || cue.sound == "S_GENASTEP2" ? 1U : 0U;
        }
        if (critters.lookoutOf(*id) != first) {
            reached = i;
        }
    }
    REQUIRE(reached > 0);
    REQUIRE(critters.moveOf(*id) == "WALK");
    REQUIRE(steps >= 1);
    REQUIRE(glm::distance(critters.positionOf(*id), post) < 1.5f);
    REQUIRE(critters.lookoutOf(*id) == route.next[static_cast<usize>(first)]);
    // Back again: the round has no end while nobody comes.
    bool back = false;
    for (s32 i = 0; i < 1500 && !back; ++i) {
        critters.update(kTicks, kStep, nobody);
        for (const CombatCue& cue : critters.takeCues()) {
            steps += cue.sound == "S_GENASTEP1" || cue.sound == "S_GENASTEP2" ? 1U : 0U;
        }
        back = critters.lookoutOf(*id) == first;
    }
    REQUIRE(back);
    REQUIRE(steps >= 2);
    REQUIRE(critters.patrolling(*id));
}

TEST_CASE("G4 generals patrol native geometry and acquire players approaching from either side",
          "[critter-patrol][g4-generals][assets]") {
    const usize instanceIndex = GENERATE(65U, 84U, 97U, 129U);
    const bool behind = GENERATE(false, true);
    CAPTURE(instanceIndex, behind);
    const auto root = test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G4")));
    const auto& instance = world.layout().itemInstances()[instanceIndex];
    const auto floor = world.collision().floorAt(instance.position, 4, 10, 1);
    REQUIRE(floor);
    Critters critters;
    critters.open(device, root, &world.collision(), EnemyScales{}, 'G');
    critters.setLookouts(LookoutRoute::of(world.layout().locators()));
    const f32 sight = instanceIndex <= 84 ? 10.0f : 15.0f;
    const auto id = critters.spawn(CombatantKind::General,
                                   Vec3{instance.position.x, floor->y, instance.position.z},
                                   instance.rotation.y, "", sight);
    REQUIRE(id);
    REQUIRE(critters.patrolling(*id));
    const s32 first = critters.lookoutOf(*id);
    bool reached = false;
    for (s32 i = 0; i < 1200 && !reached; ++i) {
        critters.update(kTicks, kStep, {});
        reached = critters.lookoutOf(*id) != first;
    }
    REQUIRE(reached);
    const Vec3 spot = critters.positionOf(*id);
    const f32 yaw = critters.yawOf(*id);
    const Vec3 direction{std::sin(yaw), 0, std::cos(yaw)};
    // CritterCalcTargetScore doubles distance behind/aside, not a front-only cone.
    const Vec3 target = spot + direction * sight * (behind ? -0.3f : 0.6f);
    for (s32 i = 0; i < 30 && critters.targetOf(*id) < 0; ++i) {
        critters.update(kTicks, kStep, std::vector{playerAt(target)});
    }
    CHECK(critters.targetOf(*id) == 0);
    CHECK_FALSE(critters.patrolling(*id));
}

TEST_CASE("G4 General platform scenario starts on the neighboring stationary floor",
          "[scenario][g4-generals][assets]") {
    const auto scenario = Scenario::load(test::dataDirectory().parent_path() /
                                         "tests/scenarios/level-g4-general-platform.json");
    REQUIRE(scenario.level == "G4");
    REQUIRE(scenario.tower.position);
    const auto party = scenario.partyMembers();
    REQUIRE(party.size() == 1);
    CHECK_FALSE(party.front().slot);
    CHECK(party.front().save.character == classIndexOf("KNI").value());
    CHECK(party.front().save.color == colorIndexOf("GRE").value());
    const auto root = test::assetOrSkip("CRITTER/GENERAL.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G4")));
    const Vec3 start = *scenario.tower.position;
    const auto floor = world.collision().floorAt(start, 0.1f, 0.1f, 1);
    REQUIRE(floor);
    CHECK(floor->object == 1091);
    CHECK(glm::distance(world.collision().resolveWalls(start, 1.0f, start.y + 0.1f, start.y + 5.0f),
                        start) < 0.01f);
    const auto& general = world.layout().itemInstances()[97];
    CHECK(glm::distance(start, general.position) < 25.0f);
}

} // namespace
