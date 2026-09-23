#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gdl::game {

/**
 * What a character gathers for the tower and its bosses, beyond the crystals: the
 * runestones (one bit each, thirteen in all), the legend items (a bit per realm, each found
 * in another realm and spent on the boss of its own), the bosses' shards (one from each
 * beaten, by the realm's place in the tower's order), and the gargoyle pieces (the
 * serpent's, the eagle's and the lion's, counted up to what the tower's statues want).
 */
struct Relics {
    static constexpr int kRuneCount = 13;
    static constexpr int kRealmCount = 16;
    static constexpr std::size_t kGargoyleKinds = 3;
    /** How many pieces of each kind the statues want. */
    static constexpr std::array<int, kGargoyleKinds> kGargoyleNeeded{12, 20, 28};

    std::uint16_t runes = 0;
    std::uint16_t legends = 0;
    std::uint16_t shards = 0; ///< the bosses' shards, a bit per realm in the tower's order
    std::array<int, kGargoyleKinds> gargoylePieces{};

    bool hasShard(int order) const {
        return inRange(order, kRealmCount) && (shards & bit(order)) != 0;
    }
    /** Takes a boss's shard; false when it was already held (or is no realm). */
    bool addShard(int order);

    bool hasRune(int rune) const { return inRange(rune, kRuneCount) && (runes & bit(rune)) != 0; }
    /** Takes the rune; false when it was already held (or is no rune). */
    bool addRune(int rune);
    int runeCount() const;

    bool hasLegend(int realm) const {
        return inRange(realm, kRealmCount) && (legends & bit(realm)) != 0;
    }
    bool addLegend(int realm);
    /** Uses the item up; false when it was not held. */
    bool spendLegend(int realm);

    /** Adds a piece of `kind`; the count now, or -1 for no such kind. */
    int addGargoylePiece(int kind);
    bool gargoyleComplete(int kind) const {
        return inRange(kind, static_cast<int>(kGargoyleKinds)) &&
               gargoylePieces[static_cast<std::size_t>(kind)] >=
                   kGargoyleNeeded[static_cast<std::size_t>(kind)];
    }

    bool operator==(const Relics&) const = default;

private:
    static bool inRange(int index, int count) { return index >= 0 && index < count; }
    static std::uint16_t bit(int index) {
        return static_cast<std::uint16_t>(1U << static_cast<unsigned int>(index));
    }
};

} // namespace gdl::game
