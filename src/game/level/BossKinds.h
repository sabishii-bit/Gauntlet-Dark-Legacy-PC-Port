#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/** The original's boss types, continuing its enemy types (e_e_tpye) after the general: the
 * codes LEVL +0x44 and the boss levels' ENMY rows store. Stored fields keep s32, with -1 for
 * none. */
namespace boss_type {
inline constexpr s32 kDragon = 34;     ///< DRAGON
inline constexpr s32 kChimera = 35;    ///< CHIMERA
inline constexpr s32 kDjinn = 36;      ///< DJINN
inline constexpr s32 kDrider = 37;     ///< DRIDER, the spider queen
inline constexpr s32 kPlagueBoss = 38; ///< PBOSS
inline constexpr s32 kYeti = 39;
inline constexpr s32 kWraith = 40;
inline constexpr s32 kLich = 41;
inline constexpr s32 kSkorne1 = 42; ///< the temple's Skorne
inline constexpr s32 kSkorne2 = 43; ///< the underworld's Skorne
inline constexpr s32 kGarm = 44;    ///< Garm himself; enemy_type::kGarm2 is his brood
} // namespace boss_type

inline constexpr s32 kFirstBossType = boss_type::kDragon;
inline constexpr s32 kLastBossType = boss_type::kGarm;
inline constexpr usize kBossTypeCount = static_cast<usize>(kLastBossType - kFirstBossType + 1);

/** Whether `code` names one of the bosses. */
[[nodiscard]] constexpr bool isBossType(s32 code) {
    return code >= kFirstBossType && code <= kLastBossType;
}

/** What a boss's level does about it, beneath the encounter itself. */
struct BossStage {
    s32 kind = -1;
    std::string_view name;          ///< MONSTERS/<name> and critter/<name>.json
    std::string_view hiddenObject;  ///< the world mesh its SFXX arena cue hides; empty: none
    std::string_view fireHoleSound; ///< heard in place of S_FIREHOLE in its level; empty: none
    char swarmBankVariant = '\0';   ///< byte 14 of its level's swarm sound banks; '\0': none
    bool fallingMasonry = false;    ///< its altar's loose masonry falls at its entrance
    bool roams = false;             ///< an unpressed close attack may start at it (PlayerMotion)
};

/** The stage of the boss a stored code names, or null for -1 and every other code. */
[[nodiscard]] const BossStage* bossStageOf(s32 code);
/** The stage whose name is `name` (a normalized asset name), or null. */
[[nodiscard]] const BossStage* bossStageNamed(std::string_view name);
/** The asset name of the boss `code` names, or empty. */
std::string_view bossNameOf(s32 code);
/** The stage mesh hidden by an SFXX arena cue, or empty when the boss has none.
 * This is the named world's object, not a mesh in the boss's animation tree. */
std::string_view bossArenaObject(s32 code);

} // namespace gdl::game
