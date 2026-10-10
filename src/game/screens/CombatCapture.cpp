#include "game/screens/CombatCapture.h"

#include <algorithm>
#include <cmath>

#include "game/screens/PlayerHealth.h"
#include "game/screens/PortalDeparture.h"

namespace gdl::game {
namespace {
CombatAnimation animation(const AnimationPlayer& player, u32 action) {
    if (!player.playing()) {
        return {};
    }
    return {action, player.sequence(), player.generation(), player.frame(), player.transition()};
}
ReplicaPlayerLife life(PlayerLife value) {
    switch (value) {
    case PlayerLife::Standing: return ReplicaPlayerLife::Standing;
    case PlayerLife::Dying: return ReplicaPlayerLife::Dying;
    case PlayerLife::InTower: return ReplicaPlayerLife::InTower;
    }
    return ReplicaPlayerLife::InTower;
}
} // namespace

std::optional<CombatSnapshot> CombatCapture::capture(const MotionSnapshot& motion,
                                                     std::span<const PlayerRuntime> players,
                                                     const Enemies& enemies,
                                                     const PortalDeparture* departure) {
    if (!motion.valid() || players.size() > InputCommand::kSeats) {
        return std::nullopt;
    }
    CombatSnapshot result;
    result.motion = motion;
    for (const auto& runtime : players) {
        const s32 player = runtime.actor.player();
        if (player < 0 || player >= static_cast<s32>(InputCommand::kSeats)) {
            return std::nullopt;
        }
        const auto seat = static_cast<usize>(player);
        const auto& shown = motion.players[seat];
        if (runtime.departed) {
            if (shown) {
                return std::nullopt;
            }
            continue;
        }
        if (!shown || result.players[seat] || shown->position != runtime.actor.position() ||
            std::abs(std::remainder(shown->yaw - runtime.actor.yaw(), kTwoPi)) > 0.0001f) {
            return std::nullopt;
        }
        PlayerCombatState state;
        state.life = life(runtime.life);
        state.health = runtime.life == PlayerLife::Standing
                           ? std::max(0.0f, static_cast<f32>(runtime.actor.save().health()) +
                                                runtime.healthFraction)
                           : 0;
        state.hitFlash = runtime.hitFlashTicks > 0;
        state.damageable = PlayerHealth::canBeDamaged(runtime);
        if (runtime.figure) {
            const auto& animator = runtime.figure->animator();
            state.animation = animation(animator.player(), static_cast<u32>(animator.action()));
            if (runtime.life != PlayerLife::InTower &&
                (runtime.life != PlayerLife::Standing || departure == nullptr ||
                 !departure->finished())) {
                const auto worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
                Mat4 base = runtime.capture.body().value_or(runtime.actor.transform());
                if (runtime.life == PlayerLife::Standing && departure != nullptr) {
                    base = departure->transform(base);
                }
                const Mat4 body = PlayerFigure::bodyPlacement(base, runtime.actor.save(), worn);
                const auto visuals = runtime.figure->companionVisuals(
                    body, worn.bodyAlpha() * runtime.transport.alpha());
                for (usize slot = 0; slot < visuals.size(); ++slot) {
                    if (const auto& visual = visuals[slot]) {
                        state.companions[slot] = CompanionState{
                            visual->form,
                            visual->placement,
                            {0, visual->sequence, visual->generation, visual->frame, 1},
                            visual->textureClock,
                            visual->alpha};
                    }
                }
            }
        }
        result.players[seat] = state;
    }
    for (s32 slot = 0; slot < Enemies::kMost; ++slot) {
        const auto observed = enemies.observe(slot);
        if (!observed) {
            continue;
        }
        EnemyCombatState state;
        state.instance = observed->instance;
        state.kind = static_cast<u32>(enemies.kindOf(slot));
        state.tier = static_cast<u32>(enemies.tierOf(slot));
        state.variant = static_cast<u32>(enemies.variantOf(slot));
        state.life = observed->asleep ? ReplicaEnemyLife::Asleep : ReplicaEnemyLife::Active;
        if (!enemies.alive(slot)) {
            state.life = ReplicaEnemyLife::Dying;
        }
        state.health = std::max(0.0f, observed->health);
        state.fullHealth = observed->fullHealth;
        state.hitFlash = observed->hitFlash;
        state.position = enemies.positionOf(slot);
        state.yaw = std::remainder(enemies.yawOf(slot), kTwoPi);
        if (const auto* animator = enemies.animatorOf(slot)) {
            state.animation = animation(animator->player(), static_cast<u32>(animator->action()));
        }
        result.enemies.push_back(state);
    }
    std::ranges::sort(result.enemies, {}, &EnemyCombatState::instance);
    return result.valid() ? std::optional{std::move(result)} : std::nullopt;
}

} // namespace gdl::game
