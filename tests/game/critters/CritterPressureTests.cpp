#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "formats/CritterWad.h"
#include "game/bosses/BossDefinition.h"
#include "game/critters/Combatant.h"
#include "game/critters/CritterPressure.h"
#include "game/critters/HeadedBodyFixture.h"
#include "game/enemies/Enemies.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

EnemyView player(s32 identity, s32 level = 1) {
    EnemyView result;
    result.player = identity;
    result.level = level;
    return result;
}

TEST_CASE("encounter pressure uses authored over current health with native stepped floors",
          "[game][critter-pressure][native-boss-rate]") {
    CritterPressure pressure;
    const std::array party{player(0)};
    pressure.refresh(party);
    // Boss e088785c/e08878a4. No damage to the party means ratio 1, 2, 4, 10;
    // it does not begin at the ordinary lost-health rate of approximately .5.
    CHECK(pressure.rate(6000, 6000, 3000) == 1);
    CHECK(pressure.rate(6000, 3000, 3000) == 2);
    CHECK(pressure.rate(6000, 1500, 3000) == 4);
    CHECK(pressure.rate(6000, 600, 3000) == 5);
    pressure.note(0, 3000);
    struct Case {
        f32 health;
        f32 expected;
    };
    const std::array cases{Case{6000, 0.25f}, Case{3000.1f, 0.25f}, Case{3000, 1},
                           Case{1500.1f, 1},  Case{1500, 2.5f},     Case{600.1f, 2.5f},
                           Case{600, 5},      Case{0, 5},           Case{-1, 5}};
    for (const Case test : cases) {
        INFO("current health " << test.health);
        CHECK(pressure.rate(6000, test.health, 3000) == test.expected);
    }
}

TEST_CASE("encounter damage accumulates by seat and only active seats enter the rate",
          "[game][critter-pressure][native-boss-rate]") {
    CritterPressure pressure;
    std::array party{player(0), player(3)};
    pressure.refresh(party);
    pressure.note(0, 600);
    pressure.note(3, 900);
    CHECK(pressure.activePlayers == 9);
    CHECK(pressure.rate(6000, 3000, 3000) == 1.5f);
    // Shared encounter damage applies to linked body parts with their OWN TYPE
    // health/reference. A child is not a second independent damage ledger.
    CHECK(pressure.rate(1000, 500, 1000) == 1);
    party[1].hidden = true;
    pressure.refresh(party);
    CHECK(pressure.rate(6000, 3000, 3000) == Approx(1.6f));
    CHECK(pressure.attemptedDamage[3] == 900);
    party[1].hidden = false;
    pressure.refresh(party);
    CHECK(pressure.rate(6000, 3000, 3000) == 1.5f);
    pressure.refresh({});
    CHECK(pressure.rate(6000, 3000, 3000) == 1);
    pressure.refresh(party);
    // There is no 15-second decay; refreshing participation preserves totals.
    for (s32 tick = 0; tick < 1800; ++tick) {
        pressure.refresh(party);
    }
    CHECK(pressure.rate(6000, 3000, 3000) == 1.5f);
}

TEST_CASE("encounter level bonus freezes once before wake and follows the native reference",
          "[game][critter-pressure][native-boss-rate]") {
    struct Case {
        f32 reference;
        f32 expected;
    };
    // Native TYPE+0xb8 from Dragon, Yeti, Plague, Chimera, Skorne and final Skorne.
    const std::array cases{Case{1000, 1.73f}, Case{2500, 1.355f}, Case{1500, 1.605f},
                           Case{2000, 1.48f}, Case{3000, 1.23f},  Case{5000, 0.73f}};
    const std::array party{player(0, 99), player(2, 99)};
    for (const Case test : cases) {
        INFO("authored damage reference " << test.reference);
        CritterPressure pressure;
        pressure.refresh(party);
        pressure.capture(party, test.reference);
        REQUIRE(pressure.levelBonus.has_value());
        CHECK(*pressure.levelBonus == Approx(test.expected));
        CHECK(pressure.rate(6000, 6000, test.reference) == Approx(1 + test.expected));
        // The bonus is added AFTER the maximum-five pressure clamp.
        CHECK(pressure.rate(6000, 1, test.reference) == Approx(5 + test.expected));
        pressure.capture(std::array{player(1, 1)}, test.reference);
        CHECK(*pressure.levelBonus == Approx(test.expected));
    }
    CritterPressure low;
    low.capture(std::array{player(0, 10)}, 1000);
    REQUIRE(low.levelBonus.has_value());
    CHECK(*low.levelBonus == 0);
    CritterPressure empty;
    empty.capture({}, 1000);
    empty.capture(party, 1000);
    REQUIRE(empty.levelBonus.has_value());
    CHECK(*empty.levelBonus == 0);
}

TEST_CASE("pressure participation is independent of invisibility and damage immunity",
          "[game][critter-pressure][native-boss-rate]") {
    auto invisible = player(1, 50);
    invisible.invisible = true;
    invisible.damageable = false;
    invisible.captured = true;
    auto departed = player(2, 99);
    departed.hidden = true;
    const std::array party{player(0, 10), invisible,      player(1, 99),
                           departed,      player(-1, 99), player(4, 99)};
    CritterPressure pressure;
    pressure.refresh(party);
    CHECK(pressure.activePlayers == 3);
    pressure.capture(party, 1000);
    REQUIRE(pressure.levelBonus.has_value());
    CHECK(*pressure.levelBonus == Approx(0.35f)); // average of 10 and 50, threshold 12.5
    // Native contact tally receives authored damage even if armor/immunity made
    // actual health loss zero. The caller, not this helper, chooses contact events.
    pressure.note(1, 500);
    CHECK(pressure.rate(1000, 1000, 1000) == Approx(1.10f));
}

TEST_CASE("pressure rejects invalid event identities and amounts without losing valid state",
          "[game][critter-pressure][native-boss-rate]") {
    CritterPressure pressure;
    pressure.refresh(std::array{player(0)});
    pressure.note(0, 100);
    const CritterPressure saved = pressure;
    pressure.note(-1, 100);
    pressure.note(4, 100);
    pressure.note(0, -1);
    pressure.note(0, 0);
    pressure.note(0, std::numeric_limits<f32>::quiet_NaN());
    pressure.note(0, std::numeric_limits<f32>::infinity());
    CHECK(pressure == saved);
    CHECK(pressure.rate(1000, 1000, 0) == 1);
    CHECK(pressure.rate(1000, 1000, -1) == 1);
    CHECK(pressure.rate(1000, std::numeric_limits<f32>::quiet_NaN(), 1000) == 1);
    pressure.note(0, std::numeric_limits<f32>::max());
    pressure.note(0, std::numeric_limits<f32>::max());
    CHECK(std::isfinite(pressure.attemptedDamage[0]));
    CHECK(pressure.rate(1000, 1000, 1000) == 0.25f);
    CritterPressure tiny;
    tiny.refresh(std::array{player(0)});
    tiny.note(0, 1000);
    CHECK(tiny.rate(1000, std::numeric_limits<f32>::denorm_min(), 1000) == 5);
}

TEST_CASE("pressure rewind values cover damage participation and the frozen level bonus",
          "[game][critter-pressure][native-boss-rate]") {
    const auto digest = [](const CritterPressure& pressure) {
        NativeStateDigest result;
        result.value(pressure);
        return result.checkedFinish();
    };
    CritterPressure pressure;
    const auto initial = digest(pressure);
    REQUIRE(initial.has_value());
    pressure.refresh(std::array{player(0, 99)});
    CHECK(digest(pressure) != initial);
    const auto roster = digest(pressure);
    pressure.capture(std::array{player(0, 99)}, 3000);
    CHECK(digest(pressure) != roster);
    const CritterPressure saved = pressure;
    const auto captured = digest(pressure);
    pressure.note(0, 100);
    CHECK(digest(pressure) != captured);
    pressure = saved;
    CHECK(digest(pressure) == captured);
    CHECK(pressure.rate(6000, 6000, 3000) == Approx(2.23f));
}

TEST_CASE("linked combatants share raw contact pressure without crediting projectile emissions",
          "[game][combatant][critter-pressure][native-boss-rate]") {
    bool enabled = true;
    bool immune = false;
    bool projectile = false;
    bool grab = false;
    SECTION("ordinary damage") {}
    SECTION("invulnerable contacts still contribute authored pressure") {
        immune = true;
    }
    SECTION("projectile emissions do not contribute contact pressure") {
        projectile = true;
    }
    SECTION("grab releases contribute authored contact damage once") {
        grab = true;
    }
    SECTION("default GDL rules retain an empty pressure ledger") {
        enabled = false;
    }
    CAPTURE(enabled, immune, projectile, grab);

    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, test::headedBodyAssets(), bossDefinition("CHIMERA"), 'G'));
    formats::CritterFile source;
    source.descriptors.push_back({"CHIMERA", "BODY", 4});
    source.types.resize(3);
    for (usize index = 0; index < source.types.size(); ++index) {
        auto& type = source.types[index];
        type.maxHealth = index == 0 ? 1000.0f : 100.0f;
        type.radius = 2;
        type.moveCount = static_cast<s16>(index == 0 ? 1 : 2);
        type.childIndex = index < 2 ? static_cast<s16>(index + 1) : s16{-1};
        type.parentIndex = static_cast<s16>(index == 0 ? -1 : 0);
        if (index > 0) {
            type.rootNode = index == 1 ? "HEAD_L" : "HEAD_R";
        }
    }
    formats::CritterMoveRecord ready;
    ready.name = "READY";
    ready.anim = "READY";
    ready.type = MoveDefinition::kReady;
    ready.priority = 512;
    ready.interrupt = 20;
    auto strike = ready;
    strike.name = "CONTACT";
    strike.anim = "SPIT";
    strike.type = 128;
    strike.priority = 544;
    strike.damage0 = 0;
    strike.colnode = "BODY";
    strike.frameEnd = 5;
    if (grab) {
        strike.frameEnd = 2;
        strike.damage1 = 0;
        strike.frameStart2 = 3;
        strike.frameEnd2 = 5;
    }
    strike.target.maxDistance = 100;
    source.moves = {ready, strike};
    formats::CritterDamageRecord damage;
    damage.type = projectile ? AttackDefinition::kProjectile
                  : grab     ? AttackDefinition::kGrab
                             : AttackDefinition::kBlow;
    damage.damage = 100;
    damage.offset = {0, 3, 0};
    damage.maxDistance = 10;
    source.damages.push_back(damage);
    CritterRules rules;
    rules.pressureReferenceDamage = enabled ? 1000.0f : 0;
    assets.data = CritterData::fromDecoded("CHIMERA", source, 0, rules);
    for (usize index = 0; index < assets.children.size(); ++index) {
        assets.children[index] = CritterData::fromDecoded("CHIMERA", source, index + 1, rules);
    }
    Combatant actor;
    REQUIRE(actor.spawn(assets, 100, Vec3{0}, 0, nullptr, {}, 'G'));
    auto target = player(0);
    target.position = {0, 0, 3};
    target.damageable = !immune;
    const std::array party{target};
    actor.captureEncounterPlayers(party);
    REQUIRE(actor.childCount() == 2);
    CHECK(actor.pressure().levelBonus.has_value() == enabled);
    Combatant::LocalSnapshot before;
    REQUIRE(actor.saveLocal(before));
    usize blows = 0;
    usize shots = 0;
    usize releases = 0;
    for (s32 tick = 0; tick < 90; ++tick) {
        actor.update(2, 1.0f / 30, party);
        blows += actor.takeBlows().size();
        for (const auto& event : actor.takeGrabs()) {
            if (!event.attachment && event.damage > 0) {
                ++releases;
            }
        }
        const auto emitted = actor.takeShots();
        shots += emitted.size();
        for (const auto& shot : emitted) {
            CHECK(shot.rate == 1); // native full-health pressure, no level bonus at level one
        }
    }
    const CritterPressure pressure = actor.pressure();
    if (enabled && !projectile) {
        REQUIRE(pressure.attemptedDamage[0] > 0);
        CHECK(pressure.attemptedDamage[1] == 0);
        CHECK(pressure.attemptedDamage[2] == 0);
        CHECK(pressure.attemptedDamage[3] == 0);
        if (immune) {
            CHECK(blows == 0);
        } else if (grab) {
            REQUIRE(releases > 0);
            CHECK(pressure.attemptedDamage[0] == static_cast<f32>(releases) * 100);
        } else {
            CHECK(pressure.attemptedDamage[0] == static_cast<f32>(blows) * 100);
        }
    } else {
        CHECK(pressure.attemptedDamage == std::array<f32, 4>{});
        if (projectile) {
            REQUIRE(shots > 0);
        } else {
            REQUIRE(blows > 0);
        }
    }
    // The local copy includes all linked pressure transactions: it replays from
    // before contact and returns to after it.
    Combatant::LocalSnapshot after;
    REQUIRE(actor.saveLocal(after));
    REQUIRE_FALSE(actor.equalLocal(before, after));
    REQUIRE(actor.restoreLocal(before));
    Combatant::LocalSnapshot now;
    REQUIRE(actor.saveLocal(now));
    CHECK(actor.equalLocal(now, before));
    REQUIRE(actor.restoreLocal(after));
    REQUIRE(actor.saveLocal(now));
    CHECK(actor.equalLocal(now, after));
    actor.clear();
    CHECK(actor.pressure() == CritterPressure{});
}
} // namespace
