#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "game/players/ClassData.h"
#include "game/players/Inventory.h"
#include "game/players/Relics.h"

namespace gdl::game {

inline constexpr std::int32_t kMaxLevel = 99;
inline constexpr std::int32_t kStartingHealth = 500;
inline constexpr std::int32_t kMaxStat = 999;

/** Experience needed to reach `level`. */
std::int32_t levelExperience(std::int32_t level);

/** The level `experience` has reached, from 1. */
std::int32_t experienceLevel(std::int32_t experience);

/** A character's progress with one class; the bonuses are added to the displayed stats. */
/** The realms the tower keeps records for, the tower itself first. */
inline constexpr std::size_t kRealmCount = 14;

struct ClassProgress {
    std::int32_t experience = 0;
    std::int32_t health = 0;
    float fightAdd = 0.0f;
    float armorAdd = 0.0f;
    float magicAdd = 0.0f;
    float speedAdd = 0.0f;
    std::array<std::int32_t, kRealmCount> crystals{}; ///< gathered towards each realm's gate
    std::uint32_t unlocked = 0; ///< a bit per realm whose gate's opening was announced
    Inventory inventory;        ///< the keys, potions and powerups carried as this class
    Relics relics;              ///< the runestones, legend items and gargoyle pieces gathered
};

/** Stats as the select screen shows them, in its row order. */
struct StatBlock {
    static constexpr std::size_t kCount = 4;
    std::array<std::int32_t, kCount> values{}; ///< strength, speed, armor, magic

    std::int32_t strength() const { return values[0]; }
    std::int32_t speed() const { return values[1]; }
    std::int32_t armor() const { return values[2]; }
    std::int32_t magic() const { return values[3]; }

    /** The row with the highest value; the first when tied. */
    std::size_t best() const;
};

/** The stats a class shows at `level` with the bonuses in `progress`, each capped at 999. */
StatBlock displayStats(const ClassStats& stats, std::int32_t level, const ClassProgress& progress);

/** What every stat reads for the hidden master class. */
StatBlock masteryStats();

} // namespace gdl::game
