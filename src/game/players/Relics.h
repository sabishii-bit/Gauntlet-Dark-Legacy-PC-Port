#pragma once

#include <array>

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * What a character gathers for the tower and its bosses, beyond the crystals: the
 * runestones (one bit each, thirteen in all), the legend items (a bit per realm, each found
 * in another realm and spent on the boss of its own), the bosses' shards (one from each
 * beaten, by the realm's place in the tower's order), and the gargoyle pieces (the
 * serpent's, the eagle's and the lion's, counted up to what the tower's statues want).
 */
struct Relics {
    static constexpr s32 kRuneCount = 13;
    static constexpr s32 kRealmCount = 16;
    static constexpr usize kGargoyleKinds = 3;
    /** How many pieces of each kind the statues want. */
    static constexpr std::array<s32, kGargoyleKinds> kGargoyleNeeded{12, 20, 28};

    u16 runes = 0;
    u16 legends = 0;
    u16 shards = 0; ///< the bosses' shards, a bit per realm in the tower's order
    std::array<s32, kGargoyleKinds> gargoylePieces{};

    bool hasShard(s32 order) const {
        return inRange(order, kRealmCount) && (shards & bit(order)) != 0;
    }
    /** Takes a boss's shard; false when it was already held (or is no realm). */
    bool addShard(s32 order);

    bool hasRune(s32 rune) const { return inRange(rune, kRuneCount) && (runes & bit(rune)) != 0; }
    /** Takes the rune; false when it was already held (or is no rune). */
    bool addRune(s32 rune);
    s32 runeCount() const;

    bool hasLegend(s32 realm) const {
        return inRange(realm, kRealmCount) && (legends & bit(realm)) != 0;
    }
    bool addLegend(s32 realm);
    /** Uses the item up; false when it was not held. */
    bool spendLegend(s32 realm);

    /** Adds a piece of `kind`; the count now, or -1 for no such kind. */
    s32 addGargoylePiece(s32 kind);
    bool gargoyleComplete(s32 kind) const {
        return inRange(kind, static_cast<s32>(kGargoyleKinds)) &&
               gargoylePieces[static_cast<usize>(kind)] >=
                   kGargoyleNeeded[static_cast<usize>(kind)];
    }

    bool operator==(const Relics&) const = default;

private:
    static bool inRange(s32 index, s32 count) { return index >= 0 && index < count; }
    static u16 bit(s32 index) { return static_cast<u16>(1U << static_cast<u32>(index)); }
};

} // namespace gdl::game
