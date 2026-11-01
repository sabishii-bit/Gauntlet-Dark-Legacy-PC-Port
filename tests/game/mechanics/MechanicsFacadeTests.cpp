#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/combat/ArmorFlags.h"
#include "game/mechanics/MechanicsFacade.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/MeleeStreak.h"
#include "game/players/PowerupEffects.h"
#include "game/players/StreakGrades.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr std::array kSources{BlowSource::Swing,  BlowSource::Shot,  BlowSource::SuperShot,
                              BlowSource::Potion, BlowSource::Turbo, BlowSource::Charge,
                              BlowSource::Breath, BlowSource::Shield};
constexpr std::array kOutcomes{BlowOutcome::Hurt, BlowOutcome::Killed, BlowOutcome::Destroyed};

TEST_CASE("out of Aggressor Mode the original's rules stand: swings alone count, as the original "
          "counted them, the run has its two seconds and anything harms a generator",
          "[game][mechanics][aggressor]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    const MechanicsFacade off{{.aggressorMode = false}};
    CHECK_FALSE(off.aggressorMode());
    CHECK_FALSE(off.meterShown());
    CHECK_FALSE(off.swarmAggravated());
    for (const BlowSource source : kSources) {
        for (const BlowOutcome outcome : kOutcomes) {
            CAPTURE(static_cast<int>(source), static_cast<int>(outcome));
            const bool original = source == BlowSource::Swing && outcome != BlowOutcome::Destroyed;
            CHECK(off.runEffect(source, outcome, players[0], players) ==
                  (original ? RunEffect::CountAsOriginal : RunEffect::Nothing));
        }
        CHECK(off.generatorTakes(source));
    }
    for (const s32 count : {0, 1, 50, 100, 500}) {
        CHECK(off.quietWindow(count) == MeleeStreak::kOriginalQuiet);
    }
}

TEST_CASE("in Aggressor Mode a kill or a generator destroyed counts one, nothing else does, the "
          "run's quiet shrinks with the grade, and only swings and shots harm a generator",
          "[game][mechanics][aggressor]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    const MechanicsFacade on{{.aggressorMode = true}};
    CHECK(on.aggressorMode());
    CHECK(on.meterShown());
    CHECK(on.swarmAggravated());
    // Unless switched on, the mode is off: a hook handed no facade plays the original.
    CHECK_FALSE(MechanicsFacade{}.aggressorMode());
    CHECK_FALSE(MechanicsFacade::standard().aggressorMode());
    for (const BlowSource source : kSources) {
        CAPTURE(static_cast<int>(source));
        CHECK(on.runEffect(source, BlowOutcome::Hurt, players[0], players) == RunEffect::Nothing);
        CHECK(on.runEffect(source, BlowOutcome::Killed, players[0], players) == RunEffect::Count);
        CHECK(on.runEffect(source, BlowOutcome::Destroyed, players[0], players) ==
              RunEffect::Count);
        CHECK(on.generatorTakes(source) ==
              (source == BlowSource::Swing || source == BlowSource::Shot));
    }
    // Three seconds at D, three eighths less a grade, one and a half at S.
    CHECK(on.quietWindow(0) == StreakGrades::kLongestQuiet);
    CHECK(on.quietWindow(1) == StreakGrades::kLongestQuiet);
    CHECK(on.quietWindow(StreakGrades::kCounts[1] - 1) == StreakGrades::kLongestQuiet);
    CHECK(on.quietWindow(StreakGrades::kCounts[1]) == Approx(2.625f));
    CHECK(on.quietWindow(StreakGrades::kCounts[2]) == Approx(2.25f));
    CHECK(on.quietWindow(StreakGrades::kCounts[3]) == Approx(1.875f));
    CHECK(on.quietWindow(StreakGrades::kTop) == StreakGrades::kShortestQuiet);
    CHECK(on.quietWindow(1000) == StreakGrades::kShortestQuiet);
    CHECK(StreakGrades::kLongestQuiet == MeleeStreak::kQuiet); // the longest a run survives
    MechanicsFacade switched;
    switched.configure({.aggressorMode = false});
    CHECK_FALSE(switched.aggressorMode());
    CHECK(switched.settings() == MechanicsFacade::Settings{.aggressorMode = false});
}

TEST_CASE("kills under time stop, invulnerability or invisibility count nothing",
          "[game][mechanics][aggressor]") {
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 5}, 0);
    const MechanicsFacade on{{.aggressorMode = true}};
    REQUIRE(on.runEffect(BlowSource::Shot, BlowOutcome::Killed, players[0], players) ==
            RunEffect::Count);
    REQUIRE(on.runEffect(BlowSource::Shot, BlowOutcome::Killed, players[1], players) ==
            RunEffect::Count);
    // Invulnerable, the first earns nothing; the second is not affected.
    players[0].actor.save().progress().inventory.addPowerup(powerup::kArmor, armor::kInvulnerable,
                                                            0, 1);
    CHECK(on.runEffect(BlowSource::Swing, BlowOutcome::Killed, players[0], players) ==
          RunEffect::Nothing);
    CHECK(on.runEffect(BlowSource::Swing, BlowOutcome::Killed, players[1], players) ==
          RunEffect::Count);
    // Invisible, the second earns nothing either.
    players[1].actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kInvisible,
                                                            0, 1);
    CHECK(on.runEffect(BlowSource::Turbo, BlowOutcome::Killed, players[1], players) ==
          RunEffect::Nothing);
    // Time stopped by anyone, nobody's kills count.
    std::array<PlayerRuntime, 2> stopped;
    stopped[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    stopped[1].actor.spawn(1, {}, nullptr, Vec3{0, 0, 5}, 0);
    stopped[1].actor.save().progress().inventory.addPowerup(powerup::kSpecial, powerup::kStopTime,
                                                            0, 1);
    CHECK(on.runEffect(BlowSource::Swing, BlowOutcome::Killed, stopped[0], stopped) ==
          RunEffect::Nothing);
    CHECK(on.runEffect(BlowSource::Swing, BlowOutcome::Destroyed, stopped[1], stopped) ==
          RunEffect::Nothing);
    // Out of the mode the original's swing counts regardless.
    const MechanicsFacade off{{.aggressorMode = false}};
    CHECK(off.runEffect(BlowSource::Swing, BlowOutcome::Hurt, stopped[1], stopped) ==
          RunEffect::CountAsOriginal);
}

} // namespace
