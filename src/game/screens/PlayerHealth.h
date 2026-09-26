#pragma once
#include <functional>
#include <random>
#include <string_view>

#include "engine/core/Types.h"

#include "game/players/ClassData.h"
#include "game/players/PlayerImpact.h"
#include "game/screens/PlayerRuntime.h"
namespace gdl::game {
/** What hurt a character, which picks how it cries out. */
enum class HurtKind : u8 {
    Blow,      ///< cries out once enough has been taken
    Burn,      ///< always cries out
    Pierce,    ///< groans
    Gas,       ///< coughs
    QuietBlow, ///< accumulates pain; the attacker supplies the impact sound, without a cry
    DeathDrain ///< bypasses ordinary armor without knockback or impact audio
};

/** Health/death rules and pain/low-health cue selection. Owns the party-wide cry RNG
 * and alternating last-health announcements; rendering and audio are synchronous outputs.
 * Like the scene it replaces, this cue sequence persists across level reopenings. */
class PlayerHealth {
public:
    static constexpr s32 kHitFlashTicks = 4;
    struct Events {
        std::function<void(f32, f32)> block;
        std::function<void(std::string_view)> sound;
        std::function<void(std::string_view)> cry;
        std::function<void(std::string_view)> named;
    };
    void hurt(PlayerRuntime& runtime, f32 damage, HurtKind kind, bool directed, bool inTower,
              f32 damageScale, const Events& events, const PlayerImpact& impact = {},
              bool bossEncounter = false, const ClassStats* stats = nullptr);
    static f32 guarded(const PlayerRuntime& runtime, f32 damage, bool directed);

private:
    void cryPain(const Events& events);
    std::mt19937 m_painRandom{0x5A17u};
    u32 m_lowHealthTurn = 0;
};
} // namespace gdl::game
