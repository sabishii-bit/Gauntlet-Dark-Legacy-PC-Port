#include "game/party/PartyDamage.h"

#include <algorithm>
#include <utility>

namespace gdl::game {
namespace {
constexpr f32 kBlockLessonAfter = 60;
constexpr f32 kBlockWorth = 2;
constexpr f32 kBlockPerDamage = 0.01f;
constexpr f32 kBlockLeast = 0.333f;
constexpr f32 kBlockMost = 1;
} // namespace

std::optional<PartyDamage::EnemyImpact> PartyDamage::enemy(const EnemyBlow& blow,
                                                           const PlayerRuntime& player,
                                                           std::span<const LevelEnemy> roster,
                                                           s32 bossType) {
    if (player.actor.player() != blow.player || !PlayerHealth::canBeDamaged(player)) {
        return std::nullopt;
    }
    auto sound = EnemyFeedback::meleeSound(blow.kind, blow.tier, roster, bossType);
    const HurtKind kind = sound ? HurtKind::QuietBlow : HurtKind::Blow;
    return EnemyImpact{std::move(sound), kind, {blow.flags, blow.direction}};
}

void PartyDamage::hurt(PlayerHealth& health, PlayerRuntime& player, f32 damage, HurtKind kind,
                       HurtSource source, const Context& context,
                       const PlayerHealth::Events& events, const PlayerImpact& impact,
                       const ClassStats* stats) {
    if (context.cutscene) {
        return;
    }
    const bool stood = player.life == PlayerLife::Standing;
    auto dispatch = events;
    dispatch.learnBlock = [&] {
        if (context.playSeconds > kBlockLessonAfter && events.learnBlock) {
            events.learnBlock();
        }
    };
    health.hurt(player, damage, kind, source, context.setting, dispatch, impact, stats);
    if (stood && player.life == PlayerLife::Dying && context.lastRealmLevel) {
        player.entrySave.progress().levels.recordBossDeath(context.realm);
    }
}

std::optional<f32> PartyDamage::block(PlayerRuntime& player, f32 taken, f32 left) {
    if (player.blockLeft > 0 || taken <= kBlockWorth) {
        return std::nullopt;
    }
    const f32 shown = std::clamp(kBlockPerDamage * left, kBlockLeast, kBlockMost);
    player.blockLeft = shown;
    player.blocked = true;
    return shown;
}

} // namespace gdl::game
