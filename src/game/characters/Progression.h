#pragma once

#include <array>

#include "engine/core/Types.h"

#include "expansions/shared/characters/LegendsProgress.h"
#include "game/characters/ClassData.h"
#include "game/characters/Inventory.h"
#include "game/characters/LevelRecord.h"
#include "game/characters/Relics.h"

namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {
struct CharacterSave;
}

namespace gdl::game {

inline constexpr s32 kMaxLevel = 99;
inline constexpr s32 kStartingHealth = 500;
inline constexpr s32 kMaxStat = 999;

/** Experience needed to reach `level`. */
s32 levelExperience(s32 level);

/** The level `experience` has reached, from 1. */
s32 experienceLevel(s32 experience);

/** A character's progress with one class; the bonuses are added to the displayed stats. */
/** The realms the tower keeps records for, the tower itself first. */
inline constexpr usize kRealmCount = 14;

/** Per-class P_SAVE_STATS counters used by shop_show_final_stats. Old saves
 * start these at zero; wallet balance and experience cannot reconstruct them. */
struct LifetimeStats {
    bool operator==(const LifetimeStats&) const = default;
    s32 enemiesKilled = 0;
    s32 generatorsDestroyed = 0;
    s32 goldFound = 0;
    f64 playSeconds = 0;
    s32 bestStreak = 0; ///< the longest run of close blows, counted as MeleeStreak does
};

struct ClassProgress {
    bool operator==(const ClassProgress&) const = default;
    s32 experience = 0;
    s32 gold = 0; ///< banked wallet; the selected class uses CharacterSave::gold while playing
    s32 promotedLevel =
        -1; ///< last tower award; -1 imports an existing character at its earned tier
    s32 health = 0;
    f32 fightAdd = 0.0f;
    f32 armorAdd = 0.0f;
    f32 magicAdd = 0.0f;
    f32 speedAdd = 0.0f;
    std::array<s32, kRealmCount> crystals{}; ///< gathered towards each realm's gate
    u32 unlocked = 0;                        ///< a bit per realm whose gate's opening was announced
    Inventory inventory; ///< the keys, potions and powerups carried as this class
    Relics relics;       ///< the runestones, legend items and gargoyle pieces gathered
    LevelRecord levels;  ///< the levels beaten and the boss levels died on
    LifetimeStats lifetime;
    std::array<LegendsProgress, kLegendsPlatformCount> legends;
    const LegendsProgress& legendsProgress(LegendsPlatform platform) const;
    LegendsProgress& legendsProgress(LegendsPlatform platform);
    s32 appearanceLevel() const;
    bool promotionPending() const;
};

/** Stats as the select screen shows them, in its row order. */
struct StatBlock {
    static constexpr usize kCount = 4;
    std::array<s32, kCount> values{}; ///< strength, speed, armor, magic

    s32 strength() const { return values[0]; }
    s32 speed() const { return values[1]; }
    s32 armor() const { return values[2]; }
    s32 magic() const { return values[3]; }

    /** The row with the highest value; the first when tied. */
    usize best() const;
};

/** The stats a class shows at `level` with the bonuses in `progress`, each capped at 999. */
StatBlock displayStats(const ClassStats& stats, s32 level, const ClassProgress& progress);

/** What every stat reads for the hidden master class. */
StatBlock masteryStats();

/** The stats a character plays with: the master's are all 999 whatever `stats` says
 * (PlayerUpdateAtts), the rest `displayStats` at the save's level with its bonuses. */
StatBlock characterStats(const ClassStats& stats, const CharacterSave& save);

/** Flat damage absorption from the current armor attribute, including level and shop gains. */
f32 armorDefense(const ClassStats& stats, const ClassProgress& progress);

/** The hidden master's armor stays at 999, independent of the underlying wizard class. */
f32 armorDefense(const ClassStats& stats, const CharacterSave& save);

void visitNativeState(NativeStateDigest& digest, const LifetimeStats& value);
void visitNativeState(NativeStateDigest& digest, const ClassProgress& value);

} // namespace gdl::game
