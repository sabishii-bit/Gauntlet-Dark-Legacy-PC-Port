#include <limits>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/combat/PowerupFlags.h"
#include "game/enemies/EnemyKinds.h"
#include "game/hud/HelpMessages.h"
#include "game/party/AttackCheckpointFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Boundary = PlayerAttacksCheckpoint;
static_assert(!std::is_copy_constructible_v<PlayerAttacks>);
static_assert(!std::is_move_assignable_v<PlayerAttacks>);

struct NativeAttackHistory {
    PlayerAttacks::LocalSnapshot attacks;
    PlayerArsenal::LocalSnapshot arsenal;
    EffectTrees::LocalSnapshot effects;
    PlayerFigure::LocalSnapshot figure;
    PlayerSimulation player;
    BodyGlow glow;
    AmbientDimmer dimmer;

    void save(const test::AttackCheckpointFixture& source) {
        source.attacks.saveLocal(attacks);
        source.arsenal.saveLocal(arsenal);
        source.effects.saveLocal(effects);
        source.players[0].figure->saveLocal(figure);
        player = source.players[0].simulation();
        glow = source.players[0].glow;
        dimmer = source.dimmer;
    }
    void restore(test::AttackCheckpointFixture& target) const {
        REQUIRE(target.attacks.canRestoreLocal(attacks));
        REQUIRE(target.arsenal.canRestoreLocal(arsenal));
        REQUIRE(target.effects.canRestoreLocal(effects));
        REQUIRE(target.players[0].figure->canRestoreLocal(figure));
        REQUIRE(target.attacks.restoreLocal(attacks));
        REQUIRE(target.arsenal.restoreLocal(arsenal));
        REQUIRE(target.effects.restoreLocal(effects));
        REQUIRE(target.players[0].figure->restoreLocal(figure));
        target.players[0].simulation() = player;
        target.players[0].glow = glow;
        target.dimmer = dimmer;
    }
};

TEST_CASE("native attack values rewind paid potions shields breath and speculative effect births",
          "[rollback][local-rewind][player-attacks-local-rewind]") {
    const s32 kind = GENERATE(0, 1, 2, 3);
    test::AttackCheckpointFixture uninterrupted;
    test::AttackCheckpointFixture replay;
    NativeAttackHistory history;
    constexpr f32 kSeconds = 1.0f / 30;
    for (auto* fixture : {&uninterrupted, &replay}) {
        fixture->players[0].actor.save().progress().inventory.addPowerup(
            powerup::kSpecial, powerup::kFireBreath, 100, 30, true);
    }
    const auto step = [&](test::AttackCheckpointFixture& fixture, s32 tick) {
        auto& player = fixture.players[0];
        player.actor.place({static_cast<f32>(tick % 80) * 0.01f, 0, 0});
        player.figure->animate(
            0, 1, kSeconds, kind == 2 && tick % 90 == 0 ? PlayerDeed::Breathe : PlayerDeed::None);
        fixture.effects.capturePresentation();
        if (tick % 90 == 0) {
            // Unknown-color potions advance the arsenal's own selection cycle.
            player.actor.save().progress().inventory.addPotions(0, 2);
            if (kind == 0) {
                fixture.attacks.usePotion(0, fixture.players);
            } else if (kind == 1) {
                fixture.attacks.shieldPotion(0, fixture.players);
            } else if (kind == 3) {
                fixture.attacks.shatterPotion(0, {1, 2, 3});
            }
        }
        if (kind == 2) {
            fixture.attacks.useItemAttack(0, fixture.players);
        }
        fixture.attacks.updateStrikes(kSeconds, fixture.players, fixture.targets);
        fixture.attacks.updateProjectiles(kSeconds, fixture.players, fixture.targets);
        fixture.attacks.updateShields(kSeconds, fixture.players, fixture.targets);
        fixture.effects.update(kSeconds);
    };
    usize births = 0;
    usize retirements = 0;
    s32 frame = 0;
    for (s32 window = 0; window < 96; ++window) {
        const s32 length = 1 + window % 8;
        CAPTURE(kind, window, length, frame);
        history.save(replay);
        const auto saved = replay.state();
        const auto beforeVisuals = replay.effectState();
        const auto preceding = uninterrupted.effectState();
        uninterrupted.helps.clear();
        replay.helps.clear();
        for (s32 i = 0; i < length; ++i) {
            step(uninterrupted, frame + i);
            replay.players[0].actor.save().progress().inventory.addPotions(0, 2);
            replay.attacks.shieldPotion(0, replay.players);
            replay.attacks.shatterPotion(0, {9, 8, 7});
            step(replay, frame + i + 19);
        }
        REQUIRE(replay.state() != saved);
        const auto rejected = replay.helps;
        const auto uploads = replay.device.texturesCreated;
        const auto updates = replay.device.textureUpdates;
        history.restore(replay);
        CHECK(replay.helps == rejected);
        CHECK(replay.state() == saved);
        CHECK(replay.effectState() == beforeVisuals);
        CHECK(replay.device.texturesCreated == uploads);
        CHECK(replay.device.textureUpdates == updates);
        replay.helps.clear();
        for (s32 i = 0; i < length; ++i) {
            step(replay, frame + i);
        }
        CHECK(replay.state() == uninterrupted.state());
        const auto shown = uninterrupted.effectState();
        CHECK(replay.effectState() == shown);
        CHECK(replay.helps == uninterrupted.helps);
        CHECK(replay.players[0].actor.checkpoint() == uninterrupted.players[0].actor.checkpoint());
        CHECK(replay.players[0].figure->animator().checkpoint() ==
              uninterrupted.players[0].figure->animator().checkpoint());
        births += shown.nextInstance > preceding.nextInstance ? 1U : 0U;
        retirements += std::ranges::any_of(preceding.effects,
                                           [&](const auto& effect) {
                                               return std::ranges::none_of(
                                                   shown.effects, [&](const auto& live) {
                                                       return live.instance == effect.instance;
                                                   });
                                           })
                           ? 1U
                           : 0U;
        frame += length;
    }
    CHECK(births >= 4);
    CHECK(retirements >= 3);
    const auto unrelated = uninterrupted.state();
    CHECK_FALSE(uninterrupted.attacks.restoreLocal(history.attacks));
    CHECK(uninterrupted.state() == unrelated);
    replay.attacks.clear();
    CHECK_FALSE(replay.attacks.canRestoreLocal(history.attacks));
    CHECK_FALSE(replay.attacks.restoreLocal(history.attacks));
    replay.attacks.bind({replay.device, replay.classes, replay.world, replay.weapons,
                         replay.effects, replay.audio, nullptr, replay.arsenal, replay.dimmer});
    CHECK_FALSE(replay.attacks.restoreLocal(history.attacks));
}

TEST_CASE(
    "attack checkpoint resumes native potion shield and item windows without replaying payment",
    "[player-attacks-checkpoint]") {
    const s32 kind = GENERATE(0, 1, 2, 3);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    CAPTURE(kind, seconds);
    test::AttackCheckpointFixture fixture;
    auto& player = fixture.players[0];
    auto& inventory = player.actor.save().progress().inventory;
    inventory.addPotions(2, 3);
    if (kind == 0) {
        fixture.attacks.usePotion(0, fixture.players);
    } else if (kind == 1) {
        fixture.attacks.shieldPotion(0, fixture.players);
    } else if (kind == 2) {
        inventory.addPowerup(powerup::kSpecial, powerup::kFireBreath, 3, 30, true);
        player.figure->animate(0, 1, seconds, PlayerDeed::Breathe);
        REQUIRE(player.figure->animator().itemReleased() == PlayerDeed::Breathe);
        fixture.attacks.useItemAttack(0, fixture.players);
    } else {
        fixture.attacks.shatterPotion(4, {1, 2, 3});
    }
    const auto saved = fixture.state();
    const auto visual = fixture.effectState();
    REQUIRE(fixture.attacks.pendingGameplay());
    REQUIRE_FALSE(visual.effects.empty());
    if (kind == 2) {
        REQUIRE(saved.items.size() == 1);
        CHECK(saved.items[0].item == Boundary::Item::FireBreath);
    }
    const auto paid = inventory;
    const auto health = player.actor.save().health();
    const auto uploads = fixture.device.texturesCreated;
    fixture.restore(saved, visual);
    CHECK(inventory == paid);
    CHECK(player.actor.save().health() == health);
    CHECK(fixture.helps.empty());
    CHECK(fixture.effectState() == visual); // restore never stops shared borrowed visuals
    CHECK(fixture.device.texturesCreated == uploads);

    std::vector<Boundary::State> expected;
    std::vector<EffectTrees::PoolCheckpoint> shown;
    const auto step = [&](s32 tick) {
        player.actor.place({static_cast<f32>(tick) * 0.01f, 0, 0});
        fixture.attacks.updateStrikes(seconds, fixture.players, fixture.targets);
        fixture.attacks.updateProjectiles(seconds, fixture.players, fixture.targets);
        fixture.attacks.updateShields(seconds, fixture.players, fixture.targets);
        fixture.effects.update(seconds);
    };
    for (s32 tick = 0; tick < 200; ++tick) {
        CAPTURE(tick);
        step(tick);
        expected.push_back(fixture.state());
        shown.push_back(fixture.effectState());
    }
    REQUIRE_FALSE(fixture.attacks.pendingGameplay());
    const auto expectedHelps = fixture.helps;
    CHECK(expectedHelps ==
          (kind == 0 ? std::vector<s32>{HelpMessages::kWastedMagic} : std::vector<s32>{}));
    fixture.restore(saved, visual);
    fixture.helps.clear();
    for (s32 tick = 0; tick < 200; ++tick) {
        CAPTURE(tick, kind, seconds);
        step(tick);
        CHECK(fixture.state() == expected[static_cast<usize>(tick)]);
        CHECK(fixture.effectState() == shown[static_cast<usize>(tick)]);
    }
    CHECK(fixture.helps == expectedHelps);
    CHECK(inventory == paid);
    CHECK(player.actor.save().health() == health);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("native attack rewind preserves turbo contact ledgers and enemy harm after wrong futures",
          "[rollback][local-rewind][player-attacks-local-rewind]") {
    const bool flying = GENERATE(false, true);
    test::AttackCheckpointFixture uninterrupted;
    test::AttackCheckpointFixture replay;
    for (auto* fixture : {&uninterrupted, &replay}) {
        fixture->opponents.open({fixture->device, fixture->world, fixture->weapons,
                                 fixture->effects, fixture->audio, fixture->root, 1},
                                fixture->players);
        EnemyScales scales;
        scales.health = 100;
        auto& enemies = fixture->opponents.enemies();
        enemies.open(fixture->device, fixture->root, nullptr, 8, scales, 1);
        REQUIRE(enemies.loadKind(kGruntKind));
        REQUIRE(enemies.spawn({.kind = kGruntKind, .position = {0, 0, 5}, .placed = true}, {}));
        fixture->players[0].turbo.add(100);
    }
    NativeAttackHistory history;
    Enemies::LocalSnapshot enemies;
    constexpr f32 kSeconds = 1.0f / 60;
    const auto step = [&](test::AttackCheckpointFixture& fixture, s32 tick) {
        auto& player = fixture.players[0];
        player.figure->animate(0, 1, kSeconds,
                               tick == 0
                                   ? (flying ? PlayerDeed::TurboFull : PlayerDeed::TurboStrong)
                                   : PlayerDeed::None);
        fixture.effects.capturePresentation();
        fixture.attacks.updateTurbo(0, 1, kSeconds, fixture.players, [](s32, usize) {});
        fixture.attacks.updateStrikes(kSeconds, fixture.players, fixture.targets);
        fixture.effects.update(kSeconds);
    };
    const f32 full = uninterrupted.opponents.enemies().healthOf(0);
    bool sawContact = false;
    bool sawStrike = false;
    s32 frame = 0;
    for (s32 window = 0; window < 64; ++window) {
        const s32 length = 1 + window % 8;
        CAPTURE(flying, window, frame, length);
        history.save(replay);
        REQUIRE(replay.opponents.enemies().saveLocal(enemies));
        uninterrupted.helps.clear();
        replay.helps.clear();
        for (s32 i = 0; i < length; ++i) {
            step(uninterrupted, frame + i);
            replay.players[0].actor.place({100, 0, 100});
            replay.attacks.shatterPotion(0, {0, 0, 5});
            replay.attacks.updateProjectiles(kSeconds * 2, replay.players, replay.targets);
            step(replay, frame + i);
        }
        const auto uploads = replay.device.texturesCreated;
        REQUIRE(replay.opponents.enemies().canRestoreLocal(enemies));
        history.restore(replay);
        REQUIRE(replay.opponents.enemies().restoreLocal(enemies));
        CHECK(replay.device.texturesCreated == uploads);
        replay.helps.clear();
        for (s32 i = 0; i < length; ++i) {
            step(replay, frame + i);
        }
        const auto expected = uninterrupted.state();
        CHECK(replay.state() == expected);
        CHECK(replay.effectState() == uninterrupted.effectState());
        CHECK(replay.opponents.enemies().checkpoint() ==
              uninterrupted.opponents.enemies().checkpoint());
        CHECK(replay.players[0].actor.checkpoint() == uninterrupted.players[0].actor.checkpoint());
        CHECK(replay.players[0].turbo.held() == uninterrupted.players[0].turbo.held());
        CHECK(replay.helps == uninterrupted.helps);
        sawStrike |= !expected.strikes.strikes.empty();
        sawContact |= std::ranges::any_of(
            expected.strikeSources, [](const auto& source) { return !source.contacts.empty(); });
        frame += length;
    }
    CHECK(sawStrike);
    CHECK(sawContact);
    CHECK(uninterrupted.opponents.enemies().healthOf(0) < full);
}

TEST_CASE("attack restoration preserves native turbo repeated-contact cooldowns and their damage",
          "[player-attacks-checkpoint]") {
    const bool flying = GENERATE(false, true);
    test::AttackCheckpointFixture fixture;
    auto& enemies = fixture.opponents.enemies();
    EnemyScales scales;
    scales.health = 100;
    fixture.opponents.open({fixture.device, fixture.world, fixture.weapons, fixture.effects,
                            fixture.audio, fixture.root, 1},
                           fixture.players);
    enemies.open(fixture.device, fixture.root, nullptr, 8, scales, 1);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto enemy =
        enemies.spawn({.kind = kGruntKind, .position = {0, 0, 5}, .placed = true}, {});
    REQUIRE(enemy);
    auto& player = fixture.players[0];
    player.turbo.add(100);
    const f32 full = enemies.healthOf(*enemy);
    constexpr f32 kSeconds = 1.0f / 60;
    bool hit = false;
    for (s32 tick = 0; tick < 60 && !hit; ++tick) {
        player.figure->animate(0, 1, kSeconds,
                               tick == 0
                                   ? (flying ? PlayerDeed::TurboFull : PlayerDeed::TurboStrong)
                                   : PlayerDeed::None);
        fixture.attacks.updateTurbo(0, 1, kSeconds, fixture.players, [](s32, usize) {});
        fixture.attacks.updateStrikes(kSeconds, fixture.players, fixture.targets);
        fixture.effects.update(kSeconds);
        const auto state = fixture.state();
        hit = !state.strikeSources.empty() && !state.strikeSources[0].contacts.empty();
    }
    REQUIRE(hit);
    REQUIRE(enemies.healthOf(*enemy) < full);
    const auto saved = fixture.state();
    const auto visual = fixture.effectState();
    const auto victims = enemies.checkpoint();
    REQUIRE(victims);
    REQUIRE(saved.strikeSources[0].contacts[0].remaining > 0.25f);
    CHECK(saved.strikes.strikes[0].owner == 3); // native input slot, not party index zero
    const f32 afterFirst = enemies.healthOf(*enemy);
    const f32 paid = player.turbo.held();
    fixture.restore(saved, visual);
    CHECK(enemies.checkpoint() == victims);
    CHECK(player.turbo.held() == paid);
    std::vector<f32> health;
    std::vector<Boundary::State> expected;
    for (s32 tick = 0; tick < 240; ++tick) {
        fixture.attacks.updateStrikes(kSeconds, fixture.players, fixture.targets);
        fixture.effects.update(kSeconds);
        health.push_back(enemies.healthOf(*enemy));
        expected.push_back(fixture.state());
        if (tick < 10) {
            CHECK(health.back() == afterFirst);
        }
    }
    CHECK((health.back() < afterFirst) == flying);
    fixture.restore(saved, visual);
    REQUIRE(enemies.restore(*victims));
    for (s32 tick = 0; tick < 240; ++tick) {
        CAPTURE(tick, flying);
        fixture.attacks.updateStrikes(kSeconds, fixture.players, fixture.targets);
        fixture.effects.update(kSeconds);
        CHECK(enemies.healthOf(*enemy) == health[static_cast<usize>(tick)]);
        CHECK(fixture.state() == expected[static_cast<usize>(tick)]);
    }
    CHECK(player.turbo.held() == paid);
}

TEST_CASE("attack checkpoint resumes independent native potion shield target cooldowns",
          "[player-attacks-checkpoint][potion-shield-checkpoint]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    CAPTURE(seconds);
    test::AttackCheckpointFixture fixture;
    fixture.opponents.open({fixture.device, fixture.world, fixture.weapons, fixture.effects,
                            fixture.audio, fixture.root, 1},
                           fixture.players);
    auto& enemies = fixture.opponents.enemies();
    EnemyScales scales;
    scales.health = 100;
    enemies.open(fixture.device, fixture.root, nullptr, 8, scales, 19);
    REQUIRE(enemies.loadKind(kGruntKind));
    const auto first =
        enemies.spawn({.kind = kGruntKind, .position = {0, 0, 1}, .placed = true}, {});
    REQUIRE(first);
    auto& inventory = fixture.players[0].actor.save().progress().inventory;
    inventory.addPotions(2, 2);
    fixture.attacks.shieldPotion(0, fixture.players);
    const auto paid = inventory;
    const f32 firstFull = enemies.healthOf(*first);
    fixture.attacks.updateShields(seconds, fixture.players, fixture.targets);
    REQUIRE(enemies.healthOf(*first) < firstFull);
    const f32 firstHit = enemies.healthOf(*first);
    const auto second =
        enemies.spawn({.kind = kGruntKind, .position = {1, 0, 1}, .placed = true}, {});
    REQUIRE(second);
    const f32 secondFull = enemies.healthOf(*second);
    fixture.attacks.updateShields(seconds, fixture.players, fixture.targets);
    CHECK(enemies.healthOf(*first) == firstHit);
    REQUIRE(enemies.healthOf(*second) < secondFull);
    const f32 secondHit = enemies.healthOf(*second);
    const auto saved = fixture.state();
    const auto visual = fixture.effectState();
    const auto victims = enemies.checkpoint();
    REQUIRE(victims);
    REQUIRE(saved.shields.size() == 1);
    REQUIRE(saved.shields[0].marks.size() == 2);
    CHECK(saved.shields[0].marks[0] == Boundary::State::Shield::Mark{0, *first, 0.25f - seconds});
    CHECK(saved.shields[0].marks[1] == Boundary::State::Shield::Mark{0, *second, 0.25f});
    const auto uploads = fixture.device.texturesCreated;
    std::vector<Boundary::State> expected;
    std::vector<EffectTrees::PoolCheckpoint> shown;
    std::vector<std::array<f32, 2>> health;
    const auto step = [&] {
        fixture.attacks.updateShields(seconds, fixture.players, fixture.targets);
        fixture.effects.update(seconds);
    };
    for (s32 tick = 0; tick < 200; ++tick) {
        step();
        expected.push_back(fixture.state());
        shown.push_back(fixture.effectState());
        health.push_back({enemies.healthOf(*first), enemies.healthOf(*second)});
    }
    CHECK(health.back()[0] < firstHit);
    CHECK(health.back()[1] < secondHit);
    REQUIRE_FALSE(fixture.attacks.pendingGameplay());
    fixture.restore(saved, visual);
    REQUIRE(enemies.restore(*victims));
    for (usize tick = 0; tick < expected.size(); ++tick) {
        CAPTURE(tick);
        step();
        CHECK(fixture.state() == expected[tick]);
        CHECK(fixture.effectState() == shown[tick]);
        CHECK(enemies.healthOf(*first) == health[tick][0]);
        CHECK(enemies.healthOf(*second) == health[tick][1]);
    }
    CHECK(inventory == paid);
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.helps.empty());
}

TEST_CASE("attack preparation rejects malformed references and actor rebinding atomically",
          "[player-attacks-checkpoint]") {
    test::AttackCheckpointFixture fixture;
    fixture.players[0].actor.save().progress().inventory.addPotions(2, 2);
    fixture.attacks.usePotion(0, fixture.players);
    fixture.attacks.shieldPotion(0, fixture.players);
    CHECK_FALSE(Boundary::capture(fixture.attacks, fixture.actors));
    auto saved = fixture.state();
    const auto visual = fixture.effectState();
    // A stopped attachment can remain until its cleanup phase and must survive restore.
    saved.attachments.push_back({0, visual.effects[0].id, 0, Mat4{1}, true});
    saved.particles.push_back({0, 0, visual.effects[0].id, "BODY", {1, 2, 3}});
    saved.items.push_back({Boundary::Item::FireBreath, 0, 0, 0.2f, 2, true, {101}});
    // A mark need not still name a live target. Preserve all native families,
    // their order, and the zero-expiry boundary without inventing a live roster.
    saved.shields[0].marks = {
        {0, 17, 0}, {1, 19, 0.1f}, {2, 23, 0.2f}, {3, 0, 0.25f}, {4, 29, 0.125f}};
    fixture.restore(saved, visual);
    REQUIRE(fixture.state() == saved);
    for (s32 mutation = 0; mutation < 22; ++mutation) {
        CAPTURE(mutation);
        auto bad = saved;
        switch (mutation) {
        case 0: bad.items[0].item = static_cast<Boundary::Item>(255); break;
        case 1: bad.items[0].item = Boundary::Item::AcidBreath; break; // nonnative Pojo variant
        case 2: bad.items[0].actor = 1; break;
        case 3: bad.items[0].hit.push_back(101); break;
        case 4: bad.potions[0].impact.effectResource = 999; break;
        case 5: bad.shields[0].effect = visual.nextId; break;
        case 6: bad.shields[0].marks[0].wait = std::numeric_limits<f32>::quiet_NaN(); break;
        case 7: bad.attachments[0].local[3][3] = 0; break;
        case 8: bad.particles[0].node.assign(257, 'A'); break;
        case 9: bad.strikeSources.push_back({1, 0, 999}); break;
        case 10: bad.actors[0].player = 0; break;
        case 11: bad.actors[0].character = 2; break;
        case 12: bad.potions.resize(Boundary::State::kMaxAreas + 1); break;
        case 13: bad.shields[0].marks[0].kind = -1; break;
        case 14: bad.shields[0].marks[0].kind = 5; break;
        case 15: bad.shields[0].marks[0].id = -1; break;
        case 16: bad.shields[0].marks[3].id = 1; break;
        case 17: bad.shields[0].marks[0].wait = -0.01f; break;
        case 18: bad.shields[0].marks[0].wait = 0.2501f; break;
        case 19: bad.shields[0].marks.push_back(bad.shields[0].marks[0]); break;
        case 20: bad.shields[0].marks.resize(Boundary::State::kMaxContacts + 1); break;
        case 21: bad.shields[0].marks[0].wait = std::numeric_limits<f32>::infinity(); break;
        default: break;
        }
        CHECK_FALSE(Boundary::prepare(fixture.attacks, bad, fixture.actors, visual,
                                      fixture.missileResources));
        CHECK(fixture.state() == saved);
        CHECK(fixture.effectState() == visual);
    }
    auto a =
        Boundary::prepare(fixture.attacks, saved, fixture.actors, visual, fixture.missileResources);
    auto sibling =
        Boundary::prepare(fixture.attacks, saved, fixture.actors, visual, fixture.missileResources);
    REQUIRE(a);
    REQUIRE(sibling);
    PlayerAttacks other;
    CHECK_FALSE(Boundary::commit(other, std::move(*a)));
    REQUIRE(Boundary::commit(fixture.attacks, std::move(*a)));
    CHECK_FALSE(Boundary::canCommit(fixture.attacks, *a));
    CHECK_FALSE(Boundary::commit(fixture.attacks, std::move(*sibling)));
    a = Boundary::prepare(fixture.attacks, saved, fixture.actors, visual, fixture.missileResources);
    REQUIRE(a);
    fixture.attacks.clear();
    CHECK_FALSE(Boundary::commit(fixture.attacks, std::move(*a)));
}
} // namespace
