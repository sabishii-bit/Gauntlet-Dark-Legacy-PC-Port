#include "game/party/PlayerPowerups.h"

#include <algorithm>

#include "engine/core/Types.h"

#include "game/combat/PowerupFlags.h"
#include "game/party/PlayerFigure.h"
#include "game/players/EnemyShrink.h"
#include "game/players/PowerupEffects.h"
#include "game/players/PowerupEndings.h"

namespace gdl::game {
namespace {
/** Whether a standing player wears a working special carrying `flag`. */
bool wearsSpecial(const PlayerRuntime& player, u32 flag) {
    return player.life == PlayerLife::Standing &&
           (PowerupEffects::of(player.actor.save().progress().inventory).special & flag) != 0;
}
} // namespace

bool PlayerPowerups::timeStopped(std::span<const PlayerRuntime> players) {
    return std::ranges::any_of(players, [](const PlayerRuntime& player) {
        return wearsSpecial(player, powerup::kStopTime);
    });
}

void PlayerPowerups::restrictToChallenge(std::span<PlayerRuntime> players) {
    // activate_player discards Stop Time on zone entry. The port keeps carried
    // powerups between zones, but that must not enable it during timed coin hunts.
    for (auto& player : players) {
        for (auto& slot : player.actor.save().progress().inventory.powerups) {
            if (!powerupAllowedInChallenge(slot.kind, slot.flags)) {
                slot.on = false;
            }
        }
    }
}

s32 PlayerPowerups::enemyShrinkers(std::span<const PlayerRuntime> players) {
    return static_cast<s32>(std::ranges::count_if(players, [](const PlayerRuntime& player) {
        return wearsSpecial(player, powerup::kEnemyShrink);
    }));
}

f32 PlayerPowerups::enemyShrink(std::span<const PlayerRuntime> players, bool bossEncounter) {
    return EnemyShrink::scaleOf(enemyShrinkers(players), bossEncounter);
}

f32 PlayerPowerups::rateOf(Clock clock) {
    switch (clock) {
    case Clock::Paused: return 0.0f;
    case Clock::Level: return 1.0f;
    case Clock::BossFight: return kBossFightRate;
    }
    return 0.0f;
}

std::vector<PowerupEnding> PlayerPowerups::update(std::span<PlayerRuntime> players, f32 seconds,
                                                  Clock clock) {
    runClock(players, seconds, clock);
    return wear(players, seconds, clock);
}

void PlayerPowerups::runClock(std::span<PlayerRuntime> players, f32 seconds, Clock clock) {
    const f32 rate = rateOf(clock);
    if (rate <= 0.0f) {
        return;
    }
    for (PlayerRuntime& player : players) {
        if (player.life == PlayerLife::Standing) {
            player.actor.save().progress().inventory.advance(seconds * rate);
        }
    }
}

std::vector<PowerupEnding> PlayerPowerups::wear(std::span<PlayerRuntime> players, f32 seconds,
                                                Clock clock) {
    std::vector<PowerupEnding> endings;
    const f32 timerRate = rateOf(clock);
    for (usize i = 0; i < players.size(); ++i) {
        PlayerRuntime& player = players[i];
        auto& inventory = player.actor.save().progress().inventory;
        if (player.life != PlayerLife::Standing) {
            if (player.life == PlayerLife::Dying) {
                player.mikey.update(seconds, inventory, player.actor.followPoint(), timerRate);
            } else {
                player.mikey.clear();
            }
            continue;
        }
        player.mikey.update(seconds, inventory, player.actor.followPoint(), timerRate);
        for (auto& slot : inventory.powerups) {
            if (slot.working() && slot.kind == powerup::kSpecial &&
                (slot.flags & powerup::kTurbo) != 0) {
                player.turbo.add(TurboMeter::kFull);
                if (slot.strength >= 0) {
                    slot.strength = 0;
                    slot.on = false; // Native state3: a later pickup waits for activation again.
                }
            }
        }
        const PowerupEffects worn = PowerupEffects::of(inventory);
        const bool plainSize =
            (player.figure ? player.figure->bodySize(player.actor.save(), worn)
                           : PlayerFigure::bodyScale(player.actor.save(), worn)) == 1.0f;
        for (const std::string_view sound :
             PowerupEndings::soundsOf(player.wornSpecial, worn.special, plainSize)) {
            endings.push_back({i, sound});
        }
        player.wornSpecial = worn.special;
    }
    return endings;
}
} // namespace gdl::game
