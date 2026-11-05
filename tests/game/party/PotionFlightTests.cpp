#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/DamageTypes.h"
#include "game/party/PotionEmission.h"
#include "game/rollback/PotionIdentity.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("potion provenance distinguishes use and throw without weapon identity aliases",
          "[game][potion-flight]") {
    PotionIdentity origin{4, 100, 9, 2, 1, PotionIdentity::Kind::Throw};
    CHECK(origin.valid());
    auto use = origin;
    use.kind = PotionIdentity::Kind::Use;
    CHECK(use.valid());
    CHECK(use != origin);
    for (const u8 seat : {u8{4}, u8{255}}) {
        auto wrong = origin;
        wrong.seat = seat;
        CHECK_FALSE(wrong.valid());
    }
    auto wrong = origin;
    wrong.kind = static_cast<PotionIdentity::Kind>(2);
    CHECK_FALSE(wrong.valid());
    wrong = origin;
    wrong.epoch = 0;
    CHECK_FALSE(wrong.valid());
    wrong = origin;
    wrong.grant = 0;
    CHECK_FALSE(wrong.valid());
    wrong = origin;
    wrong.generation = 0;
    CHECK_FALSE(wrong.valid());
}

TEST_CASE("replayed potion flight keeps the native ballistic continuation and impact",
          "[game][potion-flight]") {
    const s32 kind = GENERATE(1, 2, 3, 4);
    const s32 charge = GENERATE(0, 8, 30);
    const u32 flags = GENERATE(0U, damage::kHeal);
    auto launch =
        PotionEmission::throwPotion({1, {0, 2, 0}, {0, 0, 1}, kind, 24, 1.1f, flags}, charge);
    launch.potionRelease = MissilePotionRelease{37};
    PlayerMissiles native;
    PlayerMissiles replay;
    REQUIRE(native.launch(launch));
    REQUIRE(replay.launch(launch));
    constexpr f32 kStep = 1.0f / 60;
    for (s32 tick = 0; tick < 6; ++tick) {
        native.update(kStep, nullptr);
        replay.update(kStep, nullptr);
    }
    REQUIRE(native.count() == 1);
    REQUIRE(native.visualHistory().size() == 1);
    CHECK(PlayerMissiles::flightCheckpoint(replay.missile(0)) ==
          PlayerMissiles::flightCheckpoint(native.missile(0)));
    CHECK(replay.visualHistory().front().flight == native.visualHistory().front().flight);
    CHECK(replay.missile(0).potionRelease == launch.potionRelease);
    for (s32 tick = 6; tick < 190; ++tick) {
        CAPTURE(kind, charge, tick);
        native.update(kStep, nullptr);
        replay.update(kStep, nullptr);
        REQUIRE(replay.count() == native.count());
        REQUIRE(replay.visualHistory().size() == native.visualHistory().size());
        REQUIRE(replay.visualHistory().size() == 1);
        CHECK(replay.visualHistory().front().flight == native.visualHistory().front().flight);
        CHECK(replay.visualHistory().front().potion == kind);
        CHECK(replay.visualHistory().front().potionRelease == launch.potionRelease);
        CHECK_FALSE(replay.visualHistory().front().release);
        if (native.count() != 0) {
            CHECK(PlayerMissiles::flightCheckpoint(replay.missile(0)) ==
                  PlayerMissiles::flightCheckpoint(native.missile(0)));
        }
        const auto expected = native.takeImpacts();
        const auto actual = replay.takeImpacts();
        REQUIRE(actual.size() == expected.size());
        if (!actual.empty()) {
            CHECK(actual.front().position == expected.front().position);
            CHECK(actual.front().potion == expected.front().potion);
            CHECK(actual.front().potency == expected.front().potency);
            CHECK(actual.front().flags == expected.front().flags);
            CHECK(actual.front().damage == expected.front().damage);
        }
    }
}

TEST_CASE("native potion art and private flights retain exact live and retired particle history",
          "[game][potion-flight][potion-particle-flight][assets]") {
    const s32 kind = GENERATE(1, 2, 3, 4);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const bool impact = GENERATE(false, true);
    test::FakeRenderDevice device;
    ItemArchive artwork;
    const auto weaponPath = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path();
    REQUIRE(artwork.load(weaponPath));
    auto launch =
        PotionEmission::throwPotion({2, {0, 2, 0}, {0, 0, 1}, kind, 24, 1.1f, damage::kHeal}, 8);
    launch.archive = &artwork;
    launch.potionRelease = MissilePotionRelease{51};
    PlayerMissiles native;
    native.bindVisuals(device);
    REQUIRE(native.launch(launch));
    PlayerMissiles replay;
    REQUIRE(replay.launch(launch)); // no device: resource-free native physics, not invented art
    REQUIRE(native.visualHistory().size() == 1);
    REQUIRE(replay.visualHistory().size() == 1);
    REQUIRE(native.visualHistory().front().effect != 0);
    CHECK(replay.visualHistory().front().effect == 0);
    const auto birth = native.visualHistory().front().flight;
    CHECK(birth == replay.visualHistory().front().flight);
    CHECK(birth.birth == PlayerMissiles::flightCheckpoint(native.missile(0)));
    CHECK(birth.activeSteps == 0);
    CHECK(birth.totalSteps == 0);
    CHECK_FALSE(birth.retiring);

    WorldCollision floor;
    floor.build({{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
                 {{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
    const WorldCollision* collision = impact ? &floor : nullptr;
    bool retired = false;
    bool nativeTail = false;
    std::optional<PlayerMissiles::VisualHistory> terminal;
    for (u32 tick = 1; tick <= 220; ++tick) {
        CAPTURE(kind, seconds, impact, tick);
        if (tick == 5) {
            const auto held = replay.visualHistory().front();
            const auto nativeHeld = native.visualHistory().front();
            const auto checkpoint = PlayerMissiles::flightCheckpoint(replay.missile(0));
            for (u32 wait = 0; wait < 8; ++wait) {
                native.update(0, collision);
                replay.update(0, collision);
            }
            CHECK(replay.visualHistory().front().flight == held.flight);
            CHECK(replay.visualHistory().front().placement == held.placement);
            CHECK(native.visualHistory().front().flight == nativeHeld.flight);
            CHECK(native.visualHistory().front().placement == nativeHeld.placement);
            CHECK(PlayerMissiles::flightCheckpoint(replay.missile(0)) == checkpoint);
        }
        native.update(seconds, collision);
        replay.update(seconds, collision);
        REQUIRE(native.count() == replay.count());
        REQUIRE(replay.visualHistory().size() == 1);
        const auto& actual = replay.visualHistory().front();
        REQUIRE(actual.flight.valid());
        CHECK(actual.potion == kind);
        CHECK(actual.character == 0);
        CHECK(actual.potionRelease == launch.potionRelease);
        CHECK_FALSE(actual.release);
        CHECK(actual.flight.totalSteps == tick);
        if (native.count() != 0) {
            CHECK(PlayerMissiles::flightCheckpoint(native.missile(0)) ==
                  PlayerMissiles::flightCheckpoint(replay.missile(0)));
            CHECK(actual.placement == PlayerMissiles::transformOf(replay.missile(0)));
            CHECK(actual.direction == replay.missile(0).velocity);
            CHECK(actual.flight.activeSteps == tick);
        } else if (!terminal) {
            terminal = actual;
            retired = true;
            CHECK(actual.flight.retiring);
            CHECK(actual.flight.activeSteps == tick - 1); // impact/expiry stops emission first
        } else {
            CHECK(actual.flight.activeSteps == terminal->flight.activeSteps);
            CHECK(actual.placement == terminal->placement);
            CHECK(actual.direction == terminal->direction);
        }
        if (!native.visualHistory().empty()) {
            REQUIRE(native.visualHistory().size() == 1);
            const auto& expected = native.visualHistory().front();
            CHECK(actual.flight == expected.flight);
            CHECK(actual.placement == expected.placement);
            CHECK(actual.direction == expected.direction);
            CHECK(actual.potionRelease == expected.potionRelease);
            CHECK(expected.potion == kind);
            nativeTail = nativeTail || expected.flight.retiring;
            CHECK(native.visualHistory(expected.effect) == &expected);
        }
        const auto expectedHits = native.takeImpacts();
        const auto actualHits = replay.takeImpacts();
        REQUIRE(expectedHits.size() == actualHits.size());
        if (!actualHits.empty()) {
            CHECK(actualHits.front().position == expectedHits.front().position);
            CHECK(actualHits.front().potion == kind);
        }
    }
    CHECK(retired);
    CHECK(nativeTail);
    CHECK(device.draws.empty()); // history capture and replay need no rendering
    const auto final = replay.visualHistory().front();
    for (u32 wait = 0; wait < 8; ++wait) {
        replay.update(0, collision);
    }
    CHECK(replay.visualHistory().front().flight == final.flight);
    CHECK(replay.visualHistory().front().placement == final.placement);
    CHECK(replay.visualHistory().front().potionRelease == launch.potionRelease);
}

TEST_CASE("potion history does not misclassify foreign projectile modes",
          "[game][potion-flight][potion-particle-flight]") {
    auto launch = PotionEmission::throwPotion({0, {0, 3, 0}, {0, 0, 1}, 2, 8, 1, 0}, 2);
    SECTION("wrong native spec") {
        launch.spec = &MissileSpec::of(0);
    }
    SECTION("unknown potion kind") {
        launch.potion = 5;
    }
    SECTION("weapon modifier flags") {
        launch.flags = damage::kFiveWayShot;
    }
    SECTION("conflicting weapon identity") {
        launch.release = MissileRelease{0, 3};
    }
    PlayerMissiles missiles;
    REQUIRE(missiles.launch(launch)); // provenance must never change native launch behavior
    CHECK(missiles.visualHistory().empty());
}
} // namespace
