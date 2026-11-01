#pragma once

#include <array>
#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

namespace gdl::game {
struct EnemyView;

/** Encounter-wide pressure for creatures whose tempo responds to cumulative
 * attempted contact damage. This is separate from the ordinary, expiring target
 * anger memory. Keep one value on the root: linked parts share its player totals,
 * but evaluate the rate with their own authored health and damage reference. */
struct CritterPressure {
    std::array<f32, 4> attemptedDamage{};
    u8 activePlayers = 0;
    std::optional<f32> levelBonus;

    bool operator==(const CritterPressure&) const = default;
    friend void visitNativeState(NativeStateDigest& digest, const CritterPressure& state) {
        digest.fields<3>(state);
    }

    /** Refresh participation without erasing a departed player's accumulated damage. */
    void refresh(std::span<const EnemyView> players);
    /** Freeze the level contribution once, when the encounter begins its wake timer. */
    void capture(std::span<const EnemyView> players, f32 referenceDamage);
    /** Record the authored contact amount, before armor, immunity or actual health loss. */
    void note(s32 player, f32 rawContactDamage);
    f32 rate(f32 authoredHealth, f32 currentHealth, f32 referenceDamage) const;
};
} // namespace gdl::game
