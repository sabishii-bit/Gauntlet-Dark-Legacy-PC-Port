#pragma once

#include <optional>
#include <span>
#include <string>

#include "game/enemies/Enemies.h"
#include "game/party/PlayerHealth.h"

namespace gdl::game {

/** Native damage dispatch; the scene owns sound, effect and controller callbacks. */
class PartyDamage {
public:
    struct Context {
        PlayerHealth::Setting setting;
        bool cutscene = false;
        bool lastRealmLevel = false;
        s32 realm = -1;
        f32 playSeconds = 0;
    };
    struct EnemyImpact {
        /** A present empty name still selects QuietBlow in the original path. */
        std::optional<std::string> sound;
        HurtKind kind = HurtKind::Blow;
        PlayerImpact impact;
    };

    static std::optional<EnemyImpact> enemy(const EnemyBlow& blow, const PlayerRuntime& player,
                                            std::span<const LevelEnemy> roster, s32 bossType = -1);
    static void hurt(PlayerHealth& health, PlayerRuntime& player, f32 damage, HurtKind kind,
                     HurtSource source, const Context& context, const PlayerHealth::Events& events,
                     const PlayerImpact& impact = {}, const ClassStats* stats = nullptr);
    /** Mutates the native guard ledger, returning the accepted BLOCKFX duration.
     * Resource availability is the caller's concern; the native wrapper calls
     * this only after its existing bound-services check. */
    static std::optional<f32> block(PlayerRuntime& player, f32 taken, f32 left);
};

} // namespace gdl::game
