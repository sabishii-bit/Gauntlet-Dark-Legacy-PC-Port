#include "game/screens/PlayerPowerups.h"

#include "game/players/PowerupEffects.h"

namespace gdl::game {
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
