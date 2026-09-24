#pragma once

#include <array>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

/** One powerup a character carries, the way the original keeps them. */
struct PowerupSlot {
    f32 strength = 0.0f; ///< how much is left; none frees the slot, under none it is for good
    s32 kind = 0;        ///< the item subtype it came from
    f32 charge = 0.0f;   ///< its stat boost, or the uses a charged one has left
    u32 flags = 0;       ///< which of its kind it is
    bool on = true;      ///< worn; the selector over the status box takes it off and on

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
    static constexpr s32 kMostKeys = 9;
    static constexpr s32 kMostPotions = 9;
    static constexpr usize kPowerupSlots = 11;
    static constexpr f32 kRenewShare = 0.5f; ///< of a powerup's strength, added to one held

    s32 keys = 0;
    std::vector<s32> potions; ///< the kind of each
    std::array<PowerupSlot, kPowerupSlots> powerups{};

    /** Takes what keys there is room for; returns how many. */
    s32 addKeys(s32 count);
    /** Spends a key on a lock; false with none to spend. */
    bool spendKey();
    /** Takes the next potion out to be used; its kind, or 0 with none. */
    s32 takePotion();
    /** Takes what potions of `kind` there is room for; returns how many. */
    s32 addPotions(s32 kind, s32 count);
    /** The kind of the potion that shows and is thrown next, or 0 with none. */
    s32 nextPotion() const { return potions.empty() ? 0 : potions.back(); }

    /** Takes a powerup: one already held of the same kind and flags gains its charge and
     * half its strength (or becomes for good); else it fills a free slot, or the weakest. */
    void addPowerup(s32 kind, u32 flags, f32 charge, f32 strength);
    /** The powerup of `kind` with any of `mask` that is held and switched on, or null. */
    const PowerupSlot* powerup(s32 kind, u32 mask) const;
    usize powerupCount() const;
    /** Expire enabled timed items; disabled and charged/permanent items do not tick. */
    void advance(f32 seconds);
    /** Spend one charge of an enabled item; a negative charge count is unlimited. */
    bool spendPowerup(s32 kind, u32 mask);
    /** The next held slot after `from` going by `step` (1 or -1), wrapping; -1 with none
     * held. From -1 the search starts at either end. */
    s32 nextHeld(s32 from, s32 step) const;

    bool operator==(const Inventory&) const = default;
};

} // namespace gdl::game
