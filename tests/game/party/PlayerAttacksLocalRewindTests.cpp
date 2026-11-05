#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/NativeStateDigest.h"
#include "engine/world/AmbientDimmer.h"

#include "NativeStateSupport.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/BodyGlow.h"
#include "game/effects/EffectTrees.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyKinds.h"
#include "game/hud/HelpMessages.h"
#include "game/level/NativeStateResources.h"
#include "game/party/AttackSceneFixture.h"
#include "game/party/PlayerArsenal.h"
#include "game/party/PlayerAttacks.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerDeeds.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Digests = test::AttackSceneFixture::Digests;
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

    void save(const test::AttackSceneFixture& source) {
        source.attacks.saveLocal(attacks);
        source.arsenal.saveLocal(arsenal);
        source.effects.saveLocal(effects);
        source.players[0].figure->saveLocal(figure);
        player = source.players[0].simulation();
        glow = source.players[0].glow;
        dimmer = source.dimmer;
    }
    void restore(test::AttackSceneFixture& target) const {
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

/** The attack owner's values and the shared effects they name, saved and restored together. */
struct AttackHistory {
    PlayerAttacks::LocalSnapshot attacks;
    EffectTrees::LocalSnapshot effects;

    void save(const test::AttackSceneFixture& source) {
        source.attacks.saveLocal(attacks);
        source.effects.saveLocal(effects);
    }
    void restore(test::AttackSceneFixture& target) const {
        REQUIRE(target.attacks.restoreLocal(attacks));
        REQUIRE(target.effects.restoreLocal(effects));
    }
};

/** The instances of the effects playing, to tell which were born and which retired. */
std::vector<u64> instancesOf(const EffectTrees& effects) {
    std::vector<u64> result;
    for (usize index = 0; index < effects.count(); ++index) {
        result.push_back(effects.effect(index).instance);
    }
    return result;
}

/** The swarm's native digest, its artwork named alike in every load. */
NativeStateDigest::Digest swarmDigest(const Enemies& enemies) {
    NativeStateResources resources;
    REQUIRE(enemies.registerNativeResources(resources, "stage/attacks-local-rewind/swarm"));
    return test::liveDigest(enemies, resources.resources());
}

/** The swarm's values now. */
std::unique_ptr<Enemies::LocalSnapshot> snapshotOf(const Enemies& enemies) {
    auto result = std::make_unique<Enemies::LocalSnapshot>();
    REQUIRE(enemies.saveLocal(*result));
    return result;
}

/** Whether the swarm holds exactly what `saved` does. */
bool holds(const Enemies& enemies, const Enemies::LocalSnapshot& saved) {
    return enemies.equalLocal(*snapshotOf(enemies), saved);
}

/** Whether `from` holds an instance that `among` lacks. */
bool anyMissing(std::span<const u64> from, std::span<const u64> among) {
    return std::ranges::any_of(
        from, [&](u64 instance) { return std::ranges::find(among, instance) == among.end(); });
}

TEST_CASE("native attack values rewind paid potions shields breath and speculative effect births",
          "[rollback][local-rewind][player-attacks-local-rewind]") {
    const s32 kind = GENERATE(0, 1, 2, 3);
    test::AttackSceneFixture uninterrupted;
    test::AttackSceneFixture replay;
    NativeAttackHistory history;
    constexpr f32 kSeconds = 1.0f / 30;
    for (auto* fixture : {&uninterrupted, &replay}) {
        fixture->players[0].actor.save().progress().inventory.addPowerup(
            powerup::kSpecial, powerup::kFireBreath, 100, 30, true);
    }
    const auto step = [&](test::AttackSceneFixture& fixture, s32 tick) {
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
        const auto saved = replay.digests();
        const auto preceding = instancesOf(uninterrupted.effects);
        uninterrupted.helps.clear();
        replay.helps.clear();
        for (s32 i = 0; i < length; ++i) {
            step(uninterrupted, frame + i);
            replay.players[0].actor.save().progress().inventory.addPotions(0, 2);
            replay.attacks.shieldPotion(0, replay.players);
            replay.attacks.shatterPotion(0, {9, 8, 7});
            step(replay, frame + i + 19);
        }
        REQUIRE(replay.digests().attacks != saved.attacks);
        const auto rejected = replay.helps;
        const auto uploads = replay.device.texturesCreated;
        const auto updates = replay.device.textureUpdates;
        history.restore(replay);
        CHECK(replay.helps == rejected);
        CHECK(replay.device.texturesCreated == uploads);
        CHECK(replay.device.textureUpdates == updates);
        const auto restored = replay.digests();
        CHECK(restored.attacks == saved.attacks);
        CHECK(restored.effects == saved.effects);
        replay.helps.clear();
        for (s32 i = 0; i < length; ++i) {
            step(replay, frame + i);
        }
        const auto expected = uninterrupted.digests();
        const auto replayed = replay.digests();
        CHECK(replayed.attacks == expected.attacks);
        CHECK(replayed.effects == expected.effects);
        CHECK(replay.helps == uninterrupted.helps);
        CHECK(replay.players[0].actor == uninterrupted.players[0].actor);
        CHECK(replay.players[0].figure->animator().checkpoint() ==
              uninterrupted.players[0].figure->animator().checkpoint());
        const auto shown = instancesOf(uninterrupted.effects);
        births += anyMissing(shown, preceding) ? 1U : 0U;
        retirements += anyMissing(preceding, shown) ? 1U : 0U;
        frame += length;
    }
    CHECK(births >= 4);
    CHECK(retirements >= 3);
    const auto unrelated = uninterrupted.digests().attacks;
    CHECK_FALSE(uninterrupted.attacks.restoreLocal(history.attacks));
    CHECK(uninterrupted.digests().attacks == unrelated);
    replay.attacks.clear();
    CHECK_FALSE(replay.attacks.canRestoreLocal(history.attacks));
    CHECK_FALSE(replay.attacks.restoreLocal(history.attacks));
    replay.attacks.bind({replay.device, replay.classes, replay.world, replay.weapons,
                         replay.effects, replay.audio, nullptr, replay.arsenal, replay.dimmer});
    CHECK_FALSE(replay.attacks.restoreLocal(history.attacks));
}

TEST_CASE("attack rewind resumes native potion shield and item windows without replaying payment",
          "[player-attacks-local-rewind]") {
    const s32 kind = GENERATE(0, 1, 2, 3);
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    CAPTURE(kind, seconds);
    test::AttackSceneFixture fixture;
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
    AttackHistory saved;
    saved.save(fixture);
    REQUIRE(fixture.attacks.pendingGameplay());
    REQUIRE(fixture.effects.count() != 0);
    if (kind == 2) {
        const auto items = fixture.attacks.itemAreas();
        REQUIRE(items.size() == 1);
        CHECK(items[0].attack.tree == "FIREBREATHE");
    }
    const auto paid = inventory;
    const auto health = player.actor.save().health();
    const auto uploads = fixture.device.texturesCreated;
    const auto visual = fixture.digests();
    saved.restore(fixture);
    CHECK(inventory == paid);
    CHECK(player.actor.save().health() == health);
    CHECK(fixture.helps.empty());
    CHECK(fixture.digests().effects == visual.effects); // restore never stops shared visuals
    CHECK(fixture.device.texturesCreated == uploads);

    std::vector<Digests> expected;
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
        expected.push_back(fixture.digests());
    }
    REQUIRE_FALSE(fixture.attacks.pendingGameplay());
    const auto expectedHelps = fixture.helps;
    CHECK(expectedHelps ==
          (kind == 0 ? std::vector<s32>{HelpMessages::kWastedMagic} : std::vector<s32>{}));
    saved.restore(fixture);
    fixture.helps.clear();
    for (s32 tick = 0; tick < 200; ++tick) {
        CAPTURE(tick, kind, seconds);
        step(tick);
        const auto now = fixture.digests();
        CHECK(now.attacks == expected[static_cast<usize>(tick)].attacks);
        CHECK(now.effects == expected[static_cast<usize>(tick)].effects);
    }
    CHECK(fixture.helps == expectedHelps);
    CHECK(inventory == paid);
    CHECK(player.actor.save().health() == health);
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("native attack rewind preserves turbo contact ledgers and enemy harm after wrong futures",
          "[rollback][local-rewind][player-attacks-local-rewind]") {
    const bool flying = GENERATE(false, true);
    test::AttackSceneFixture uninterrupted;
    test::AttackSceneFixture replay;
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
    const auto step = [&](test::AttackSceneFixture& fixture, s32 tick) {
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
        const auto expected = uninterrupted.digests();
        const auto replayed = replay.digests();
        CHECK(replayed.attacks == expected.attacks);
        CHECK(replayed.effects == expected.effects);
        CHECK(swarmDigest(replay.opponents.enemies()) ==
              swarmDigest(uninterrupted.opponents.enemies()));
        CHECK(replay.players[0].actor == uninterrupted.players[0].actor);
        CHECK(replay.players[0].turbo.held() == uninterrupted.players[0].turbo.held());
        CHECK(replay.helps == uninterrupted.helps);
        sawStrike |= uninterrupted.attacks.strikes().count() != 0;
        sawContact |=
            std::ranges::any_of(uninterrupted.attacks.strikeSources(),
                                [](const auto& source) { return !source.contacts.empty(); });
        frame += length;
    }
    CHECK(sawStrike);
    CHECK(sawContact);
    CHECK(uninterrupted.opponents.enemies().healthOf(0) < full);
}

TEST_CASE("attack rewind preserves native turbo repeated-contact cooldowns and their damage",
          "[player-attacks-local-rewind]") {
    const bool flying = GENERATE(false, true);
    test::AttackSceneFixture fixture;
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
        const auto sources = fixture.attacks.strikeSources();
        hit = !sources.empty() && !sources[0].contacts.empty();
    }
    REQUIRE(hit);
    REQUIRE(enemies.healthOf(*enemy) < full);
    AttackHistory saved;
    saved.save(fixture);
    const auto victims = snapshotOf(enemies);
    REQUIRE(fixture.attacks.strikeSources()[0].contacts[0].remaining > 0.25f);
    REQUIRE(fixture.attacks.strikes().count() != 0);
    CHECK(fixture.attacks.strikes().strike(0).owner == 3); // native input slot, not party index
    const f32 afterFirst = enemies.healthOf(*enemy);
    const f32 paid = player.turbo.held();
    saved.restore(fixture);
    CHECK(holds(enemies, *victims));
    CHECK(player.turbo.held() == paid);
    std::vector<f32> health;
    std::vector<NativeStateDigest::Digest> expected;
    for (s32 tick = 0; tick < 240; ++tick) {
        fixture.attacks.updateStrikes(kSeconds, fixture.players, fixture.targets);
        fixture.effects.update(kSeconds);
        health.push_back(enemies.healthOf(*enemy));
        expected.push_back(fixture.digests().attacks);
        if (tick < 10) {
            CHECK(health.back() == afterFirst);
        }
    }
    CHECK((health.back() < afterFirst) == flying);
    saved.restore(fixture);
    REQUIRE(enemies.restoreLocal(*victims));
    for (s32 tick = 0; tick < 240; ++tick) {
        CAPTURE(tick, flying);
        fixture.attacks.updateStrikes(kSeconds, fixture.players, fixture.targets);
        fixture.effects.update(kSeconds);
        CHECK(enemies.healthOf(*enemy) == health[static_cast<usize>(tick)]);
        CHECK(fixture.digests().attacks == expected[static_cast<usize>(tick)]);
    }
    CHECK(player.turbo.held() == paid);
}

TEST_CASE("attack rewind resumes independent native potion shield target cooldowns",
          "[player-attacks-local-rewind]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    CAPTURE(seconds);
    test::AttackSceneFixture fixture;
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
    AttackHistory saved;
    saved.save(fixture);
    const auto victims = snapshotOf(enemies);
    REQUIRE(fixture.attacks.shieldCount() == 1);
    const auto marks = fixture.attacks.potionShields()[0].marks;
    REQUIRE(marks.size() == 2);
    CHECK((std::tuple{marks[0].kind, marks[0].id, marks[0].wait} ==
           std::tuple{0, *first, 0.25f - seconds}));
    CHECK((std::tuple{marks[1].kind, marks[1].id, marks[1].wait} == std::tuple{0, *second, 0.25f}));
    const auto uploads = fixture.device.texturesCreated;
    std::vector<Digests> expected;
    std::vector<std::array<f32, 2>> health;
    const auto step = [&] {
        fixture.attacks.updateShields(seconds, fixture.players, fixture.targets);
        fixture.effects.update(seconds);
    };
    for (s32 tick = 0; tick < 200; ++tick) {
        step();
        expected.push_back(fixture.digests());
        health.push_back({enemies.healthOf(*first), enemies.healthOf(*second)});
    }
    CHECK(health.back()[0] < firstHit);
    CHECK(health.back()[1] < secondHit);
    REQUIRE_FALSE(fixture.attacks.pendingGameplay());
    saved.restore(fixture);
    REQUIRE(enemies.restoreLocal(*victims));
    for (usize tick = 0; tick < expected.size(); ++tick) {
        CAPTURE(tick);
        step();
        const auto now = fixture.digests();
        CHECK(now.attacks == expected[tick].attacks);
        CHECK(now.effects == expected[tick].effects);
        CHECK(enemies.healthOf(*first) == health[tick][0]);
        CHECK(enemies.healthOf(*second) == health[tick][1]);
    }
    CHECK(inventory == paid);
    CHECK(fixture.device.texturesCreated == uploads);
    CHECK(fixture.helps.empty());
}
} // namespace
