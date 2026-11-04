#include <array>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/characters/Progression.h"
#include "game/effects/EffectTrees.h"
#include "game/party/AttackCheckpointFixture.h"
#include "game/party/PlayerArsenal.h"
#include "game/party/PlayerMissiles.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Digests = test::AttackCheckpointFixture::Digests;

TEST_CASE("arsenal rewind restores the shared unspecified-potion stream without spending again",
          "[player-arsenal-local-rewind]") {
    test::AttackCheckpointFixture fixture(false);
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
    test::AttackCheckpointFixture fixture;
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
    std::vector<Digests> expected;
    const auto step = [&](s32 tick) {
        actor.place({static_cast<f32>(tick), 1, 3});
        fixture.arsenal.followCaster(actor);
        fixture.effects.update(1.0f / 30);
        return fixture.digests();
    };
    for (s32 tick = 0; tick < 100; ++tick) {
        expected.push_back(step(tick));
    }
    REQUIRE(fixture.arsenal.casts().empty());
    REQUIRE(fixture.arsenal.canRestoreLocal(saved));
    REQUIRE(fixture.effects.canRestoreLocal(visual));
    REQUIRE(fixture.arsenal.restoreLocal(saved));
    REQUIRE(fixture.effects.restoreLocal(visual));
    CHECK(actor.save().progress().inventory == paid);
    CHECK(actor.save().health() == health);
    for (s32 tick = 0; tick < 100; ++tick) {
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
    test::AttackCheckpointFixture fixture(false);
    auto& actor = fixture.players[0].actor;
    actor.save().progress().inventory.addPotions(kind, 1);
    fixture.arsenal.throwPotion(actor, 18, 49);
    auto& missiles = fixture.arsenal.missiles();
    REQUIRE(missiles.count() == 1);
    const auto& launched = missiles.missile(0);
    // The bottle's artwork is not loaded here, so its flight is read through the pool's own
    // values rather than a digest that would have to name that artwork.
    const std::array<PlayerMissiles::Resource, 1> resources{
        {{7, launched.spec, launched.model, launched.streak.texture}}};
    missiles.update(0.13f, nullptr, {});
    PlayerArsenal::LocalSnapshot saved;
    fixture.arsenal.saveLocal(saved);
    REQUIRE(missiles.count() == 1);
    const auto paid = actor.save().progress().inventory;
    std::vector<std::optional<PlayerMissiles::Checkpoint>> expected;
    const auto step = [&](s32 tick) {
        missiles.update(tick % 7 == 0 ? 0 : 1.0f / 30, nullptr, {});
        return missiles.checkpoint(resources);
    };
    for (s32 tick = 0; tick < 130; ++tick) {
        expected.push_back(step(tick));
        REQUIRE(expected.back());
    }
    REQUIRE(fixture.arsenal.restoreLocal(saved));
    for (s32 tick = 0; tick < 130; ++tick) {
        CHECK(step(tick) == expected[static_cast<usize>(tick)]);
    }
    CHECK(actor.save().progress().inventory == paid);
    missiles.clear();
    CHECK_FALSE(fixture.arsenal.canRestoreLocal(saved));
}
} // namespace
