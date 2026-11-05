#pragma once
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Mt19937.h"

#include "game/characters/ClassData.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/HurtKind.h" // IWYU pragma: export
#include "game/party/PlayInput.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerImpact.h"
namespace gdl::game {
/** Health/death rules and pain/low-health cue selection. Owns the party-wide cry RNG
 * and alternating last-health announcements; rendering and audio are synchronous outputs.
 * Like the scene it replaces, this cue sequence persists across level reopenings. */
class PlayerHealth {
    template <class Visitor>
    friend void visitNativeState(Visitor& visitor, const PlayerHealth& value) {
        const auto& [painRandom] = value;
        visitor.value(painRandom);
    }

public:
    bool operator==(const PlayerHealth&) const = default;
    static constexpr s32 kHitFlashTicks = 4;
    struct Events {
        std::function<void(f32, f32)> block;
        std::function<void(std::string_view)> sound;
        std::function<void(std::string_view)> cry;
        std::function<void(std::string_view, f32)> named; ///< a line after the name, its wait
        /** A heavy blow taken unguarded by one who has never blocked. */
        std::function<void()> learnBlock;
        std::function<void(s32)> vibrate; ///< native controller frames, not simulation ticks
        bool logDeaths = true; ///< private replay may reject a lethal result without a false log
    };
    static constexpr f32 kBlockLessonFrom = 15.0f; ///< over this a blow teaches the guard
    static constexpr u32 kHeavyFlags = damage::kHeavyKnocks;
    /** Action/partner protection shared by incoming damage and combatant contact checks.
     * Does not hide a player from targeting or replace armor's damage modifiers. */
    static bool canBeDamaged(const PlayerRuntime& runtime);
    /** PlayerMotion_FloorFX skips hurt/recovery states (native motion state >= 31).
     * This is surface-hazard protection, not invulnerability to ordinary attacks. */
    static bool canTakeSurfaceDamage(const PlayerRuntime& runtime);
    /** The scene's pre-contact surface-hurt clock, independently of whether the
     * current surface is harmful. Shared with private ordinary combat steps. */
    static void advanceSurfaceGap(PlayerRuntime& runtime, f32 seconds);
    void hurt(PlayerRuntime& runtime, f32 damage, HurtKind kind, bool directed, bool inTower,
              f32 damageScale, const Events& events, const PlayerImpact& impact = {},
              bool bossEncounter = false, const ClassStats* stats = nullptr);
    static f32 guarded(const PlayerRuntime& runtime, f32 damage, bool directed);
    /** Fells the character outright, as the last point of health going does: a point is kept
     * that the status box does not show, the turbo meter empties, and the death is heard. */
    static void fall(PlayerRuntime& runtime, const Events& events);
    /** Each fallen player's answer to the box's prompt, by seat: select waits in the tower;
     * back leaves the game when `quitOffered`, and an online match, whose party is fixed to
     * its end, does not offer it. */
    static void answerTowerPrompts(std::span<PlayerRuntime> players,
                                   std::span<const PlayInput> inputs, bool quitOffered);

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
    Mt19937 m_painRandom{0x5A17u};
};
} // namespace gdl::game
