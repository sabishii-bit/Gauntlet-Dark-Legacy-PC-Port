#pragma once

#include <array>

#include "engine/core/Types.h"

#include "game/players/ClassData.h"
#include "game/players/Inventory.h"
#include "game/players/Relics.h"

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

struct ClassProgress {
    s32 experience = 0;
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

} // namespace gdl::game
