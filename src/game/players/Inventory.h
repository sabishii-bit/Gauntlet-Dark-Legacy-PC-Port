#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gdl::game {

/** One powerup a character carries, the way the original keeps them. */
struct PowerupSlot {
    float strength = 0.0f;   ///< how much is left; none frees the slot, under none it is for good
    std::int32_t kind = 0;   ///< the item subtype it came from
    float charge = 0.0f;     ///< its stat boost, or the uses a charged one has left
    std::uint32_t flags = 0; ///< which of its kind it is
    bool on = true;          ///< worn; the selector over the status box takes it off and on

    bool held() const { return strength != 0.0f; }
    /** Whether it is doing anything: held and switched on. */
    bool working() const { return held() && on; }
    bool operator==(const PowerupSlot&) const = default;
};

/**
 * What a character carries besides gold: keys, potions (each of its kind, the last taken the
 * first thrown) and powerups, with the original's limits of nine keys, nine potions and
 * eleven powerup slots, the weakest of which gives way to a new powerup.
 */
struct Inventory {
    static constexpr std::int32_t kMostKeys = 9;
    static constexpr std::int32_t kMostPotions = 9;
    static constexpr std::size_t kPowerupSlots = 11;
    static constexpr float kRenewShare = 0.5f; ///< of a powerup's strength, added to one held

    std::int32_t keys = 0;
    std::vector<std::int32_t> potions; ///< the kind of each
    std::array<PowerupSlot, kPowerupSlots> powerups{};

    /** Takes what keys there is room for; returns how many. */
    std::int32_t addKeys(std::int32_t count);
    /** Spends a key on a lock; false with none to spend. */
    bool spendKey();
    /** Takes the next potion out to be used; its kind, or 0 with none. */
    std::int32_t takePotion();
    /** Takes what potions of `kind` there is room for; returns how many. */
    std::int32_t addPotions(std::int32_t kind, std::int32_t count);
    /** The kind of the potion that shows and is thrown next, or 0 with none. */
    std::int32_t nextPotion() const { return potions.empty() ? 0 : potions.back(); }

    /** Takes a powerup: one already held of the same kind and flags gains its charge and
     * half its strength (or becomes for good); else it fills a free slot, or the weakest. */
    void addPowerup(std::int32_t kind, std::uint32_t flags, float charge, float strength);
    /** The powerup of `kind` with any of `mask` that is held and switched on, or null. */
    const PowerupSlot* powerup(std::int32_t kind, std::uint32_t mask) const;
    std::size_t powerupCount() const;
    /** The next held slot after `from` going by `step` (1 or -1), wrapping; -1 with none
     * held. From -1 the search starts at either end. */
    std::int32_t nextHeld(std::int32_t from, std::int32_t step) const;

    bool operator==(const Inventory&) const = default;
};

} // namespace gdl::game
