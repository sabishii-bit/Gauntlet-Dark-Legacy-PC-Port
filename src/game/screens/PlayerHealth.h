#pragma once
#include <functional>
#include <optional>
#include <random>
#include <span>
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
    Gas,       ///< cloud damage: gas protection and queued pain, not the poison-trap voice
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
        std::function<void(std::string_view, f32)> named; ///< a line after the name, its wait
        /** A heavy blow taken unguarded by one who has never blocked. */
        std::function<void()> learnBlock;
    };
    static constexpr f32 kBlockLessonFrom = 15.0f; ///< over this a blow teaches the guard
    static constexpr u32 kHeavyFlags = 0x10160;    ///< knock-back, knock-down and knock-over
    void hurt(PlayerRuntime& runtime, f32 damage, HurtKind kind, bool directed, bool inTower,
              f32 damageScale, const Events& events, const PlayerImpact& impact = {},
              bool bossEncounter = false, const ClassStats* stats = nullptr);
    static f32 guarded(const PlayerRuntime& runtime, f32 damage, bool directed);

    static constexpr s32 kHeartbeatHealth = 200; ///< at or under this the heart is heard
    static constexpr std::string_view kHeartbeatSound = "S_WARN";
    /** A heartbeat to play: whose, and how loud (1 is the sound's own level). */
    struct Heartbeat {
        usize player = 0;
        f32 volume = 1.0f;
    };
    /** do_weakening's warning for `ticks` more: the standing player with the least health, at
     * two hundred or under, hears their heart beat faster and louder as it falls; never in the
     * tower nor while invulnerable. */
    static std::optional<Heartbeat> heartbeat(std::span<PlayerRuntime> players, s32 ticks,
                                              bool inTower);

private:
    void cryPain(const Events& events);
    static void landBlow(PlayerRuntime& runtime, const Events& events, u32 flags);
    std::mt19937 m_painRandom{0x5A17u};
};
} // namespace gdl::game
