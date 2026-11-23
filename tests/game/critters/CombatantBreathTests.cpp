#include <array>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/ApproximateLength.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "formats/CritterWad.h"
#include "game/critters/Combatant.h"
#include "game/critters/CombatantBreath.h"
#include "game/critters/CombatantKind.h"
#include "game/critters/CritterData.h"
#include "game/critters/Gargoyle.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** A gargoyle whose one attack breathes from its body, from six up and thirty degrees below
 * its facing, reaching twelve. */
std::filesystem::path breathingGargoyle() {
    const auto root = test::scratchDirectory("combatant-breath-push");
    const auto archive = root / "MONSTERS/GAR_EAGL";
    std::filesystem::create_directories(root / "critter");
    std::filesystem::create_directories(archive);
    writeTextFile(archive / "body.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeTextFile(archive / "objects.json", R"({"objects":[{"name":"BODY","file":"body.obj"}]})");
    writeFile(archive / "skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
      {"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
    test::convertModelFixture(archive);
    writeTextFile(archive / "animations.json", R"({"trees":[{"name":"BODY",
      "nodes":[{"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"STEP","frames":3}]}]})");
    writeTextFile(root / "critter/GAR_EAGL.json", R"({
      "descriptors":[{"prefix":"BODY","name":"GAR_EAGL","type":7}],
      "types":[{"moveCount":2,"maxHealth":100,"radius":3}],
      "moves":[{"name":"READY","anim":"STEP","type":32,"interrupt":60},
               {"name":"FIRE","anim":"STEP","type":128,"priority":20,
                "colnode":"BODY","target":{"maxDistance":50},
                "frameStart":0,"frameEnd":2,"damage0":0}],
      "damages":[{"type":4,"radius":1,"maxDistance":12,"damage":1,"pitch":0.5235988,
                  "offset":[0,6,0]}]})");
    return root;
}

TEST_CASE("breath transforms the offset but normalizes the animated forward basis",
          "[game][enemies][breath]") {
    AttackDefinition damage;
    damage.maxDistance = 30;
    damage.offset = {0, -4, -3};
    const Mat4 node = glm::scale(glm::translate(Mat4{1}, Vec3{10, 20, 30}), Vec3{2});
    const auto breath = CombatantBreath::fromNode(node, damage);
    REQUIRE(breath.origin == Vec3{10, 12, 24});
    REQUIRE(breath.end == Vec3{10, 12, 54});

    const Mat4 turned = glm::rotate(node, std::numbers::pi_v<f32> / 2, Vec3{0, 1, 0});
    const auto moved = CombatantBreath::fromNode(turned, damage);
    REQUIRE(moved.origin.x == Approx(4));
    REQUIRE(moved.origin.z == Approx(30));
    REQUIRE(moved.end.x == Approx(34));
    REQUIRE(moved.end.z == Approx(30));
}

TEST_CASE("breath applies authored yaw then pitch, including an inclined node",
          "[game][enemies][breath]") {
    AttackDefinition damage;
    damage.maxDistance = 10;
    damage.yaw = std::numbers::pi_v<f32> / 2;
    damage.pitch = std::numbers::pi_v<f32> / 6;
    const auto breath = CombatantBreath::fromNode(Mat4{1}, damage);
    REQUIRE(breath.end.x == Approx(-8.660254f));
    REQUIRE(breath.end.y == Approx(-5));
    REQUIRE(breath.end.z == Approx(0).margin(0.00001));

    Mat4 inclined{1};
    inclined[2] = Vec4{0, 0.6f, 0.8f, 0};
    damage.yaw = 0;
    const auto tipped = CombatantBreath::fromNode(inclined, damage);
    REQUIRE(tipped.end.x == 0);
    REQUIRE(tipped.end.y == Approx(10 * (0.6f * std::cos(damage.pitch) - 0.5f * 0.64f)));
    REQUIRE(tipped.end.z == Approx(10 * (0.5f * 0.48f + 0.8f * std::cos(damage.pitch))));
}

TEST_CASE("breath uses a cylinder and an unexpanded horizontal distance window",
          "[game][enemies][breath]") {
    AttackDefinition damage;
    damage.radius = 0.5f;
    damage.minDistance = 2;
    damage.maxDistance = 10;
    damage.minDot = 1; // Not a cone test: this value must not restrict collision.
    const CombatantBreath breath{{0, 3, 0}, {0, 3, 10}};
    REQUIRE(breath.touches(damage, {0, 3, 2}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 3, 1.99f}, 1, 3));
    REQUIRE(breath.touches(damage, {0, 3, 10}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 3, 10.01f}, 1, 3));
    REQUIRE(breath.touches(damage, {1.5f, 6.5f, 5}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {1.501f, 3, 5}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 6.501f, 5}, 1, 3));
    REQUIRE_FALSE(breath.touches(damage, {0, 3, -5}, 1, 3));
    // The closest point is three-dimensional, not a projection onto the ground.
    const CombatantBreath down{{0, 10, 0}, {0, 0, 10}};
    REQUIRE(down.touches(damage, {0, 5, 5}, 0.1f, 0.1f));
    REQUIRE_FALSE(down.touches(damage, {0, 0, 5}, 0.1f, 0.1f));
    damage.minDistance = 0;
    const CombatantBreath point{{0, 3, 0}, {0, 3, 0}};
    REQUIRE(point.touches(damage, {0, 3, 0}, 1, 3));
    REQUIRE_FALSE(point.touches(damage, {0, 3, 5}, 1, 3));
}

TEST_CASE("imported world-axis breath pitch acts after the posed forward and authored yaw",
          "[game][enemies][breath][native-breath]") {
    AttackDefinition damage;
    damage.maxDistance = 10;
    damage.yaw = std::numbers::pi_v<f32> / 2;
    damage.pitch = std::numbers::pi_v<f32> / 6;
    // Native yaw turns +Z into -X. World-X pitch cannot then tip it downward,
    // unlike GDL's direction-dependent horizontal perpendicular.
    const auto native = CombatantBreath::fromNode(Mat4{1}, damage, true);
    CHECK(native.end.x == Approx(-10));
    CHECK(native.end.y == Approx(0).margin(0.00001));
    CHECK(native.end.z == Approx(0).margin(0.00001));
    const auto ordinary = CombatantBreath::fromNode(Mat4{1}, damage);
    CHECK(ordinary.end.x == Approx(-8.660254f));
    CHECK(ordinary.end.y == Approx(-5));
    CHECK(ordinary.origin == native.origin);

    Mat4 posed = glm::translate(Mat4{1}, Vec3{10, 20, 30});
    posed[2] = Vec4{1.2f, 1.6f, 0, 0};
    damage.yaw = 0;
    damage.offset = {0, 0, 2};
    const auto inclined = CombatantBreath::fromNode(posed, damage, true);
    CHECK(inclined.origin.x == Approx(12.4f));
    CHECK(inclined.origin.y == Approx(23.2f));
    CHECK(inclined.origin.z == Approx(30));
    const Vec3 direction = (inclined.end - inclined.origin) / damage.maxDistance;
    CHECK(direction.x == Approx(0.6f));
    CHECK(direction.y == Approx(0.8f * std::cos(damage.pitch)));
    CHECK(direction.z == Approx(0.4f));
    CHECK(glm::length(inclined.end - inclined.origin) == Approx(damage.maxDistance));

    posed[2] = Vec4{0};
    const auto zero = CombatantBreath::fromNode(posed, damage, true);
    CHECK(zero.end == zero.origin);
}

TEST_CASE("a breath pushes flat along its beam at the fast length's reciprocal (NormalVector2D)",
          "[game][combatant][breath]") {
    const auto root = breathingGargoyle();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Gargoyle::definition(), 'G'));
    Combatant actor;
    // Three tenths across for each unit ahead, where the fast length runs about a hundredth
    // over the Euclidean one.
    const f32 yaw = std::atan2(0.3f, 1.0f);
    REQUIRE(actor.spawn(assets, 0, Vec3{0}, yaw, nullptr, {}, 'G'));
    // Six ahead, where the falling beam passes through the body.
    EnemyView player;
    player.player = 0;
    player.position = 6.0f * Vec3{std::sin(yaw), 0.0f, std::cos(yaw)};
    const std::array players{player};
    std::vector<CombatBlow> blows;
    for (s32 i = 0; i < 10 && blows.empty(); ++i) {
        actor.update(2, 1.0f / 30, players);
        blows = actor.takeBlows();
    }
    REQUIRE_FALSE(blows.empty());
    const auto node = actor.nodeTransform("BODY");
    REQUIRE(node.has_value());
    const auto beam = CombatantBreath::fromNode(*node, actor.data()->damages()[0]);
    const Vec3 way = beam.end - beam.origin;
    REQUIRE(way.y == Approx(-6.0f)); // half its reach down, at thirty degrees
    for (const CombatBlow& blow : blows) {
        REQUIRE(blow.breath);
        CHECK(blow.direction.y == 0.0f);
        CHECK(std::hypot(blow.direction.x, blow.direction.z) == Approx(1.0f).epsilon(0.03));
        // Not the Euclidean flat unit, which lies a hundredth further out.
        CHECK(glm::distance(blow.direction, approximateFlatUnit(way)) < 1e-4f);
    }
}

/** The breathing gargoyle's own data, as breathingGargoyle writes it, under `rules`. */
CritterData breathingGargoyleData(const CritterRules& rules) {
    formats::CritterFile source;
    source.descriptors.push_back({"GAR_EAGL", "BODY", static_cast<s16>(CombatantKind::Gargoyle)});
    auto& type = source.types.emplace_back();
    type.radius = 3;
    type.wallRadius = 1;
    type.maxHealth = 100;
    type.moveCount = 2;
    auto& ready = source.moves.emplace_back();
    ready.name = "READY";
    ready.anim = "STEP";
    ready.type = MoveDefinition::kReady;
    ready.interrupt = 60;
    auto& fire = source.moves.emplace_back();
    fire.name = "FIRE";
    fire.anim = "STEP";
    fire.type = MoveDefinition::kAttackFrom;
    fire.priority = 20;
    fire.colnode = "BODY";
    fire.target.maxDistance = 50;
    fire.frameEnd = 2;
    fire.damage0 = 0;
    auto& breath = source.damages.emplace_back();
    breath.type = AttackDefinition::kBreath;
    breath.radius = 1;
    breath.maxDistance = 12;
    breath.damage = 1;
    breath.pitch = 0.5235988f;
    breath.offset = {0, 6, 0};
    return CritterData::fromDecoded("GAR_EAGL", source, 0, rules);
}

TEST_CASE("a breath under breathPushFromOrigin pushes flat from where it starts, and along its "
          "beam without it",
          "[game][combatant][breath]") {
    const bool fromOrigin = GENERATE(false, true);
    CAPTURE(fromOrigin);
    const auto root = breathingGargoyle();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Gargoyle::definition(), 'G'));
    CritterRules rules;
    rules.breathPushFromOrigin = fromOrigin;
    assets.data = breathingGargoyleData(rules);
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, Vec3{0}, 0, nullptr, {}, 'G'));
    const auto fire = assets.data.moveNamed("FIRE");
    REQUIRE(fire);
    REQUIRE(actor.setMove(*fire, {.frame = 0, .target = 0}));
    // Six ahead and one and a half aside: inside the falling beam, off the line of its way.
    EnemyView player;
    player.player = 0;
    player.position = {1.5f, 0, 6};
    const std::array players{player};
    actor.update(1, 0, players);
    const auto blows = actor.takeBlows();
    REQUIRE(blows.size() == 1);
    const auto node = actor.nodeTransform("BODY");
    REQUIRE(node.has_value());
    const auto beam = CombatantBreath::fromNode(*node, assets.data.damages()[0]);
    const Vec3 fromStart{player.position.x - beam.origin.x, 0, player.position.z - beam.origin.z};
    const Vec3 away = fromStart / std::hypot(fromStart.x, fromStart.z);
    const Vec3 alongBeam = approximateFlatUnit(beam.end - beam.origin);
    REQUIRE(glm::distance(away, alongBeam) > 0.2f);
    const Vec3& direction = blows[0].direction;
    CAPTURE(direction.x, direction.y, direction.z);
    REQUIRE(blows[0].breath);
    CHECK(direction.y == 0.0f);
    CHECK(glm::distance(direction, fromOrigin ? away : alongBeam) < 1e-5f);
}

TEST_CASE("critter damage loading preserves the breath's inner distance limit",
          "[game][enemies][breath]") {
    const auto file = test::scratchDirectory("breath-damage") / "creature.json";
    writeTextFile(file,
                  R"({"name":"TEST", "types":[{"moveCount":1}], "descriptors":[{}], "moves":[{}],
                         "damages":[{"type":4,"minDistance":7.5,"maxDistance":30}]})");
    CritterData data;
    REQUIRE(data.load(file));
    REQUIRE(data.damages().size() == 1);
    REQUIRE(data.damages()[0].minDistance == 7.5f);
}
} // namespace
