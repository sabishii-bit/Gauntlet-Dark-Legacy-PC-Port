#pragma once

#include <array>

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * What a character has done in the levels, kept per class as the original keeps it in a
 * save: which level of each realm it has beaten (the tower opens a realm's next portal by
 * the one before), the runestone levels and legend-item levels it has beaten once and
 * again, and the boss levels it has died on once and again (Sumner's hints grow a passage
 * for each).
 */
struct LevelRecord {
    static constexpr usize kRealmCount = 14;
    static constexpr s32 kLevelsPerRealm = 8; ///< a bit each in a realm's byte
    static constexpr usize kPasses = 2;       ///< the first time, and the second
    static constexpr s32 kRuneCount = 13;
    static constexpr s32 kMostRealm = 15; ///< a realm's bit fits a word of sixteen

    /** A bit per level of each realm beaten, by the level's place in the realm's order. */
    std::array<u8, kRealmCount> beaten{};
    /** The runestone levels beaten, a bit per rune less one: the first time, and again. */
    std::array<u16, kPasses> runeLevels{};
    /** The legend-item levels beaten, a bit per realm of the item's boss. */
    std::array<u16, kPasses> legendLevels{};
    /** The boss levels died on, a bit per realm. */
    std::array<u16, kPasses> bossDeaths{};

    bool hasBeaten(s32 realm, s32 level) const;
    /** Records level `level` of `realm` beaten, with the runestone (from one) and the legend
     * item's realm the level holds, none at nought (towerRecordLevelBeaten). */
    void recordBeaten(s32 realm, s32 level, s32 rune = 0, s32 legend = 0);
    /** Records a death on `realm`'s boss level (playerGiveGargItem). */
    void recordBossDeath(s32 realm);

    bool operator==(const LevelRecord&) const = default;

private:
    /** Marks the first pass, or the second when the first is already marked. */
    static void pass(std::array<u16, kPasses>& passes, s32 bit);
};

} // namespace gdl::game
