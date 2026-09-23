#pragma once

#include <array>
#include <cstddef>

#include "game/players/ClassData.h"
#include "game/players/Inventory.h"
#include "game/players/Relics.h"

namespace gdl::game {

inline constexpr int kMaxLevel = 99;
inline constexpr int kStartingHealth = 500;
inline constexpr int kMaxStat = 999;

/** Experience needed to reach `level`. */
int levelExperience(int level);

/** The level `experience` has reached, from 1. */
int experienceLevel(int experience);

/** A character's progress with one class; the bonuses are added to the displayed stats. */
/** The realms the tower keeps records for, the tower itself first. */
inline constexpr std::size_t kRealmCount = 14;

struct ClassProgress {
    int experience = 0;
    int health = 0;
    float fightAdd = 0.0f;
    float armorAdd = 0.0f;
    float magicAdd = 0.0f;
    float speedAdd = 0.0f;
    std::array<int, kRealmCount> crystals{}; ///< gathered towards each realm's gate
    unsigned int unlocked = 0;               ///< a bit per realm whose gate's opening was announced
    Inventory inventory; ///< the keys, potions and powerups carried as this class
    Relics relics;       ///< the runestones, legend items and gargoyle pieces gathered
};

/** Stats as the select screen shows them, in its row order. */
struct StatBlock {
    static constexpr std::size_t kCount = 4;
    std::array<int, kCount> values{}; ///< strength, speed, armor, magic

    int strength() const { return values[0]; }
    int speed() const { return values[1]; }
    int armor() const { return values[2]; }
    int magic() const { return values[3]; }

    /** The row with the highest value; the first when tied. */
    std::size_t best() const;
};

/** The stats a class shows at `level` with the bonuses in `progress`, each capped at 999. */
StatBlock displayStats(const ClassStats& stats, int level, const ClassProgress& progress);

/** What every stat reads for the hidden master class. */
StatBlock masteryStats();

} // namespace gdl::game
