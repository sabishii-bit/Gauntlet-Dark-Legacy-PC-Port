#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/combat/DamageTypes.h"
#include "game/players/PlayerActor.h"
#include "game/players/PowerupEffects.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using State = PlayerActor::Checkpoint;

CollisionTriangle face(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& normal,
                       s32 object = -1) {
    return {normal, {a, b, c}, object};
}

void floor(std::vector<CollisionTriangle>& triangles, f32 left, f32 right, f32 leftY, f32 rightY,
           s32 object = -1) {
    const Vec3 normal = glm::normalize(Vec3{leftY - rightY, right - left, 0});
    triangles.push_back(
        face({left, leftY, -10}, {right, rightY, 10}, {right, rightY, -10}, normal, object));
    triangles.push_back(
        face({left, leftY, -10}, {left, leftY, 10}, {right, rightY, 10}, normal, object));
}

WorldCollision room() {
    std::vector<CollisionTriangle> triangles;
    floor(triangles, -20, 20, 0, 0, 0);
    triangles.push_back(face({5, 0, -10}, {5, 8, -10}, {5, 8, 10}, {-1, 0, 0}, 1));
    triangles.push_back(face({5, 0, -10}, {5, 8, 10}, {5, 0, 10}, {-1, 0, 0}, 1));
    triangles.push_back(face({-10, 0, 6}, {10, 8, 6}, {-10, 8, 6}, {0, 0, -1}, 2));
    triangles.push_back(face({-10, 0, 6}, {10, 0, 6}, {10, 8, 6}, {0, 0, -1}, 2));
    WorldCollision result;
    result.build(triangles);
    return result;
}

ClassStats stats() {
    ClassStats result;
    result.speedMin = 350;
    result.speedMax = 750;
    result.width = 1.8f;
    result.height = 5.5f;
    result.collisionY = 2.75f;
    result.attentionY = 4.5f;
    return result;
}

void same(const PlayerActor& replay, const PlayerActor& native) {
    CHECK(replay.checkpoint() == native.checkpoint());
    CHECK(replay.speed() == native.speed());
    CHECK(replay.facing() == native.facing());
    CHECK(replay.followPoint() == native.followPoint());
    CHECK(replay.attentionPoint() == native.attentionPoint());
    CHECK(replay.transform() == native.transform());
}

TEST_CASE("actor checkpoints preserve native contact order dimensions and unclamped pace",
          "[game][players][actor-checkpoint][rollback]") {
    const auto geometry = room();
    const auto tuning = stats();
    CharacterSave sourceSave;
    sourceSave.name = "SOURCE";
    PlayerActor native;
    native.spawn(2, sourceSave, &tuning, {3.5f, 0, 4.5f}, -1.25f);
    native.setPaceBonus(20);
    native.update({glm::normalize(Vec2{1, 1}), 1}, 0, 0.5f, &geometry, 1, true);
    native.slide({0.1f, 0, 0.1f}, &geometry);
    REQUIRE(native.moving());
    REQUIRE_FALSE(native.wallContacts().empty());
    const State saved = native.checkpoint();
    REQUIRE(saved.valid());
    CHECK(saved.speed < PlayerActor::kMaxSpeed);
    CHECK(saved.paceBonus == 20);
    CHECK(native.speed() == PlayerActor::kMaxSpeed);
    CHECK(saved.attentionHeight != saved.followHeight);
    for (usize index = 0; index < native.wallContacts().size(); ++index) {
        CHECK(saved.contacts[index].object == native.wallContacts()[index].object);
        CHECK(saved.contacts[index].point == native.wallContacts()[index].point);
    }

    CharacterSave recipient;
    recipient.name = "RECIP";
    recipient.character = 3;
    recipient.color = 2;
    recipient.gold = 571;
    recipient.progress().health = 123;
    recipient.progress().experience = 2345;
    recipient.progress().inventory.addPowerup(powerup::kWeapon, damage::kFiveWayShot, 7, 123);
    PlayerActor replay;
    replay.spawn(1, recipient, nullptr, {-7, 1, 2}, 2);
    const auto identity = replay.save().toJson();
    REQUIRE(replay.restore(saved));
    same(replay, native);
    CHECK(replay.player() == 1);
    CHECK(replay.save().toJson() == identity);
    native.setPaceBonus(0);
    replay.setPaceBonus(0);
    CHECK(replay.speed() == saved.speed);
    same(replay, native);
    native.clearWallContacts();
    replay.clearWallContacts();
    native.update({}, 0, 1.0f / 60, &geometry);
    replay.update({}, 0, 1.0f / 60, &geometry);
    same(replay, native);
    CHECK_FALSE(replay.moving());
    CHECK(replay.wallContacts().empty());
}

TEST_CASE("actor checkpoint validation is bounded and failure cannot partially replace live state",
          "[game][players][actor-checkpoint][rollback]") {
    const auto tuning = stats();
    PlayerActor actor;
    actor.spawn(3, {}, &tuning, {1, 2, 3}, -0.75f);
    actor.setPaceBonus(2);
    const State before = actor.checkpoint();
    const auto identity = actor.save().toJson();
    const auto reject = [&](const State& malformed) {
        CHECK_FALSE(malformed.valid());
        CHECK_FALSE(actor.restore(malformed));
        CHECK(actor.checkpoint() == before);
        CHECK(actor.player() == 3);
        CHECK(actor.save().toJson() == identity);
    };
    constexpr std::array kScalars{
        &State::yaw,    &State::speed,        &State::paceBonus,      &State::radius,
        &State::height, &State::followHeight, &State::attentionHeight};
    for (const auto scalar : kScalars) {
        for (const f32 value :
             {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::infinity(),
              -std::numeric_limits<f32>::infinity()}) {
            auto malformed = before;
            malformed.*scalar = value;
            reject(malformed);
        }
    }
    for (s32 axis = 0; axis < 3; ++axis) {
        auto malformed = before;
        malformed.position[axis] = State::kMaxCoordinate + 1;
        reject(malformed);
        malformed.position[axis] = std::numeric_limits<f32>::quiet_NaN();
        reject(malformed);
    }
    for (const auto [field, value] : std::array<std::pair<f32 State::*, f32>, 12>{
             {{&State::yaw, State::kMaxCoordinate + 1},
              {&State::paceBonus, -State::kMaxCoordinate - 1},
              {&State::speed, PlayerActor::kMinSpeed - 0.01f},
              {&State::speed, PlayerActor::kMaxSpeed + 0.01f},
              {&State::radius, 0},
              {&State::radius, 51},
              {&State::height, 0},
              {&State::height, 101},
              {&State::followHeight, -1},
              {&State::followHeight, 101},
              {&State::attentionHeight, -1},
              {&State::attentionHeight, 101}}}) {
        auto malformed = before;
        malformed.*field = value;
        reject(malformed);
    }
    auto malformed = before;
    malformed.contacts.push_back({-2, {0, 0, 0}});
    reject(malformed);
    malformed.contacts[0].object = State::kMaxObject + 1;
    reject(malformed);
    malformed.contacts[0].object = 1;
    malformed.contacts[0].point.y = std::numeric_limits<f32>::infinity();
    reject(malformed);
    malformed.contacts = {{1, {1, 2, 3}}, {1, {4, 5, 6}}};
    reject(malformed);
    malformed.contacts.clear();
    for (usize index = 0; index <= State::kMaxContacts; ++index) {
        malformed.contacts.push_back({static_cast<s32>(index), Vec3{0}});
    }
    reject(malformed);
    malformed.contacts.pop_back();
    REQUIRE(malformed.valid());
    REQUIRE(actor.restore(malformed));
    CHECK(actor.checkpoint() == malformed);
    CHECK(actor.save().toJson() == identity);
}

TEST_CASE("actor restore resumes both directions across native slopes seams and stair landings",
          "[game][players][actor-checkpoint][terrain-seams][rollback]") {
    const bool stairs = GENERATE(false, true);
    const f32 direction = GENERATE(-1.0f, 1.0f);
    CAPTURE(stairs, direction);
    std::vector<CollisionTriangle> triangles;
    if (stairs) {
        floor(triangles, -10, -2, 0, 0);
        floor(triangles, -2, 0, 0.75f, 0.75f);
        floor(triangles, 0, 2, 1.5f, 1.5f);
        floor(triangles, 2, 10, 2.25f, 2.25f);
    } else {
        floor(triangles, -10, 0, -5, 0);
        floor(triangles, 0.02f, 10, 0.01f, 5);
    }
    WorldCollision geometry;
    geometry.build(triangles);
    const f32 startX = -5 * direction;
    const f32 startY = stairs ? (direction > 0 ? 0 : 2.25f) : startX * 0.5f;
    PlayerActor native;
    native.spawn(0, {}, nullptr, {startX, startY, 0}, 0.75f);
    native.setPaceBonus(1.25f);
    PlayerActor replay;
    replay.spawn(1, {}, nullptr, {70, 0, 40}, -2);
    REQUIRE(replay.restore(native.checkpoint()));
    constexpr f32 kStep = 1.0f / 60;
    for (s32 tick = 0; tick < 96; ++tick) {
        CAPTURE(tick);
        const auto before = native.checkpoint();
        if (tick % 7 == 0) {
            // An authoritative correction replaces every local field, not only
            // feet/yaw. No velocity or fall clock is invented by restoration.
            replay.place({80, 90, 100});
            replay.setPaceBonus(-20);
            REQUIRE(replay.restore(before));
        }
        native.clearWallContacts();
        replay.clearWallContacts();
        const MoveInput move{{direction, 0}, 1};
        native.update(move, 0, kStep, &geometry, 1, (tick % 3) == 0);
        replay.update(move, 0, kStep, &geometry, 1, (tick % 3) == 0);
        CHECK(replay.fall(kStep, geometry) == native.fall(kStep, geometry));
        same(replay, native);
        // PlayerMotion shortens a climbing step by the rise: a stair riser takes most of
        // its frame, and a slope a tenth of the pace throughout.
        REQUIRE((native.position().x - before.position.x) * direction > (stairs ? 0.0f : 0.08f));
        if (!stairs) {
            CHECK(native.position().y == Approx(native.position().x * 0.5f).margin(0.02f));
        }
    }
    CHECK(native.position().x * direction > (stairs ? 4.4f : 3.9f));
    CHECK(std::abs(native.position().y - startY) > 2);
}

TEST_CASE(
    "actor restore resumes midfall and changing floor availability without resetting progress",
    "[game][players][actor-checkpoint][falling][rollback]") {
    const f32 rate = GENERATE(30.0f, 60.0f, 120.0f);
    const f32 seconds = 1 / rate;
    auto geometry = room();
    PlayerActor native;
    native.spawn(0, {}, nullptr, {0, 10, 0}, 1.25f);
    for (s32 tick = 0; tick < 7; ++tick) {
        REQUIRE(native.fall(seconds, geometry));
    }
    REQUIRE(native.position().y > 0);
    REQUIRE(native.position().y < 10);
    PlayerActor replay;
    replay.spawn(1, {}, nullptr, {-2, -3, -4}, 0);
    REQUIRE(replay.restore(native.checkpoint()));
    for (s32 tick = 0; tick < 150; ++tick) {
        CAPTURE(rate, tick);
        if (tick == 80) {
            geometry.clear(); // a disappearing support changes only the next floor query
        } else if (tick == 100) {
            std::vector<CollisionTriangle> raised;
            floor(raised, -20, 20, native.position().y + 1, native.position().y + 1);
            geometry.build(raised);
        }
        if (tick % 11 == 0) {
            replay.turnTo(17);
            REQUIRE(replay.restore(native.checkpoint()));
        }
        const f32 before = native.position().y;
        const bool nativeFalling = native.fall(seconds, geometry);
        CHECK(replay.fall(seconds, geometry) == nativeFalling);
        same(replay, native);
        CHECK(native.position().y >= before - PlayerActor::kFallSpeed * seconds - 0.00001f);
        if (tick == 79) {
            CHECK(native.position().y == 0);
        } else if (tick == 99) {
            CHECK(native.position().y < 0);
        } else if (tick == 100) {
            CHECK_FALSE(nativeFalling);
            CHECK(native.position().y == Approx(before + 1));
        }
    }
}
} // namespace
