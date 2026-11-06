#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/characters/Progression.h"
#include "game/effects/EffectTrees.h"
#include "game/party/AttackSceneFixture.h"
#include "game/party/PlayerArsenal.h"
#include "game/party/PlayerMissiles.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Digests = test::AttackSceneFixture::Digests;

TEST_CASE("arsenal rewind restores the shared unspecified-potion stream without spending again",
          "[player-arsenal-local-rewind]") {
    test::AttackSceneFixture fixture(false);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 1);
    CHECK(fixture.arsenal.resolvePotionKind(4) == 4);
    PlayerArsenal::LocalSnapshot saved;
    fixture.arsenal.saveLocal(saved);
    auto& inventory = fixture.players[0].actor.save().progress().inventory;
    inventory.addPotions(0, 2);
    const auto before = inventory;
    CHECK(fixture.arsenal.resolvePotionKind(0) == 2);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 3);
    REQUIRE(fixture.arsenal.restoreLocal(saved));
    CHECK(inventory == before);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 2);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 3);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 4);
    CHECK(fixture.arsenal.resolvePotionKind(0) == 1);
    fixture.arsenal.clear();
    PlayerArsenal::LocalSnapshot unbound;
    fixture.arsenal.saveLocal(unbound);
    CHECK(fixture.arsenal.resolvePotionKind(0) ==
          2); // native clear does not reset the colour cycle
    CHECK_FALSE(fixture.arsenal.restoreLocal(saved));
    REQUIRE(fixture.arsenal.restoreLocal(unbound));
}

TEST_CASE("arsenal rewind resumes cast attachments with the shared effects they follow",
          "[player-arsenal-local-rewind]") {
    test::AttackSceneFixture fixture;
    auto& actor = fixture.players[0].actor;
    actor.save().progress().experience = levelExperience(80);
    actor.save().progress().inventory.addPotions(2, 1);
    REQUIRE(fixture.arsenal.usePotion(actor));
    PlayerArsenal::LocalSnapshot saved;
    EffectTrees::LocalSnapshot visual;
    fixture.arsenal.saveLocal(saved);
    fixture.effects.saveLocal(visual);
    REQUIRE(fixture.arsenal.casts().size() == 1);
    REQUIRE(fixture.arsenal.casts()[0].owner == 3);
    const auto paid = actor.save().progress().inventory;
    const auto health = actor.save().health();
    const auto uploads = fixture.device.texturesCreated;
    constexpr s32 kTicks = 100;
    std::vector<Digests> expected;
    expected.reserve(kTicks);
    const auto step = [&](s32 tick) {
        actor.place({static_cast<f32>(tick), 1, 3});
        fixture.arsenal.followCaster(actor);
        fixture.effects.update(1.0f / 30);
        return fixture.digests();
    };
    for (s32 tick = 0; tick < kTicks; ++tick) {
        expected.push_back(step(tick));
    }
    REQUIRE(fixture.arsenal.casts().empty());
    REQUIRE(fixture.arsenal.canRestoreLocal(saved));
    REQUIRE(fixture.effects.canRestoreLocal(visual));
    REQUIRE(fixture.arsenal.restoreLocal(saved));
    REQUIRE(fixture.effects.restoreLocal(visual));
    CHECK(actor.save().progress().inventory == paid);
    CHECK(actor.save().health() == health);
    for (s32 tick = 0; tick < kTicks; ++tick) {
        CAPTURE(tick);
        const auto now = step(tick);
        CHECK(now.arsenal == expected[static_cast<usize>(tick)].arsenal);
        CHECK(now.effects == expected[static_cast<usize>(tick)].effects);
    }
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("arsenal rewind includes live native potion flights queues and child invalidation",
          "[player-arsenal-local-rewind]") {
    const s32 kind = GENERATE(1, 2, 3, 4);
    test::AttackSceneFixture fixture(false);
    auto& actor = fixture.players[0].actor;
    actor.save().progress().inventory.addPotions(kind, 1);
    fixture.arsenal.throwPotion(actor, 18, 49);
    auto& missiles = fixture.arsenal.missiles();
    REQUIRE(missiles.count() == 1);
    // The bottle's artwork is not loaded here, so its flight is read through the pool's own
    // values rather than a digest that would have to name that artwork.
    struct Flight {
        u64 instance = 0;
        PlayerMissiles::FlightCheckpoint motion;
        s32 potion = 0;
        f32 potency = 0;
        f32 damage = 0;
        u32 flags = 0;
        std::optional<MissilePotionRelease> potionRelease;
        bool operator==(const Flight&) const = default;
    };
    struct Flights {
        std::vector<Flight> flights;
        std::vector<PlayerMissiles::VisualHistory> history;
        bool operator==(const Flights&) const = default;
    };
    missiles.update(0.13f, nullptr, {});
    PlayerArsenal::LocalSnapshot saved;
    fixture.arsenal.saveLocal(saved);
    REQUIRE(missiles.count() == 1);
    const auto paid = actor.save().progress().inventory;
    constexpr s32 kTicks = 130;
    std::vector<Flights> expected;
    expected.reserve(kTicks);
    const auto step = [&](s32 tick) {
        missiles.update(tick % 7 == 0 ? 0 : 1.0f / 30, nullptr, {});
        Flights result;
        for (usize index = 0; index < missiles.count(); ++index) {
            const auto& missile = missiles.missile(index);
            result.flights.push_back({missile.instance, PlayerMissiles::flightCheckpoint(missile),
                                      missile.potion, missile.potency, missile.damage,
                                      missile.flags, missile.potionRelease});
        }
        const auto history = missiles.visualHistory();
        result.history.assign(history.begin(), history.end());
        return result;
    };
    for (s32 tick = 0; tick < kTicks; ++tick) {
        expected.push_back(step(tick));
    }
    REQUIRE(fixture.arsenal.restoreLocal(saved));
    for (s32 tick = 0; tick < kTicks; ++tick) {
        CHECK(step(tick) == expected[static_cast<usize>(tick)]);
    }
    CHECK(actor.save().progress().inventory == paid);
    missiles.clear();
    CHECK_FALSE(fixture.arsenal.canRestoreLocal(saved));
}
} // namespace
