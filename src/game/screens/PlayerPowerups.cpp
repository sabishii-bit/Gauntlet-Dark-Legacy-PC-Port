#include "game/screens/PlayerPowerups.h"

#include <algorithm>

#include "engine/core/Types.h"

#include "game/players/EnemyShrink.h"
#include "game/players/PowerupEffects.h"

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

s32 PlayerPowerups::enemyShrinkers(std::span<const PlayerRuntime> players) {
    return static_cast<s32>(std::ranges::count_if(players, [](const PlayerRuntime& player) {
        return wearsSpecial(player, powerup::kEnemyShrink);
    }));
}

f32 PlayerPowerups::enemyShrink(std::span<const PlayerRuntime> players, bool bossEncounter) {
    return EnemyShrink::scaleOf(enemyShrinkers(players), bossEncounter);
}

void PlayerPowerups::update(std::span<PlayerRuntime> players, f32 seconds, Clock clock) {
    for (auto& player : players) {
        if (player.life != PlayerLife::Standing) {
            continue;
        }
        auto& inventory = player.actor.save().progress().inventory;
        if (clock != Clock::Paused) {
            inventory.advance(seconds * (clock == Clock::BossFight ? 3.0f : 1.0f));
        }
        for (auto& slot : inventory.powerups) {
            if (slot.working() && slot.kind == powerup::kSpecial &&
                (slot.flags & powerup::kTurbo) != 0) {
                player.turbo.add(TurboMeter::kFull);
                if (slot.strength >= 0) {
                    slot.strength = 0;
                }
            }
        }
    }
}
} // namespace gdl::game
