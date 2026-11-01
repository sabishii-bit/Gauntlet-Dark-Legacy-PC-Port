#include "game/critters/CritterPressure.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "game/enemies/Enemies.h"

namespace gdl::game {
namespace {
bool participates(const EnemyView& player) {
    return !player.hidden && player.player >= 0 && player.player < 4;
}
} // namespace

void CritterPressure::refresh(std::span<const EnemyView> players) {
    activePlayers = 0;
    for (const EnemyView& player : players) {
        if (participates(player)) {
            activePlayers |= static_cast<u8>(1U << player.player);
        }
    }
}

void CritterPressure::capture(std::span<const EnemyView> players, f32 referenceDamage) {
    if (levelBonus.has_value()) {
        return;
    }
    // The source encounter captures the average level only once (Boss e0886000),
    // independently of later roster updates. Ignore duplicate or invalid seats.
    std::array<s32, 4> levels{};
    u8 present = 0;
    for (const EnemyView& player : players) {
        if (participates(player) && (present & (1U << player.player)) == 0) {
            levels[static_cast<usize>(player.player)] = std::max(0, player.level);
            present |= static_cast<u8>(1U << player.player);
        }
    }
    f32 sum = 0;
    f32 count = 0;
    for (usize slot = 0; slot < levels.size(); ++slot) {
        if ((present & (1U << slot)) != 0) {
            sum += static_cast<f32>(levels[slot]);
            count += 1;
        }
    }
    levelBonus = 0;
    if (count > 0 && std::isfinite(referenceDamage)) {
        *levelBonus = std::max(0.0f, (sum / count - referenceDamage / 80.0f) / 50.0f);
    }
}

void CritterPressure::note(s32 player, f32 rawContactDamage) {
    if (player < 0 || static_cast<usize>(player) >= attemptedDamage.size() ||
        !std::isfinite(rawContactDamage) || rawContactDamage <= 0) {
        return;
    }
    f32& total = attemptedDamage[static_cast<usize>(player)];
    // Valid native encounters remain far below this bound; malformed or runaway
    // input must not place an infinity into rewind state.
    total = static_cast<f32>(std::min(static_cast<f64>(total) + rawContactDamage,
                                      static_cast<f64>(std::numeric_limits<f32>::max())));
}

f32 CritterPressure::rate(f32 authoredHealth, f32 currentHealth, f32 referenceDamage) const {
    const f32 bonus = levelBonus.value_or(0);
    if (!std::isfinite(authoredHealth) || authoredHealth <= 0 || !std::isfinite(currentHealth) ||
        !std::isfinite(referenceDamage) || referenceDamage <= 0) {
        return 1 + bonus;
    }
    f32 reference = 0;
    f32 damage = 0;
    for (usize slot = 0; slot < attemptedDamage.size(); ++slot) {
        if ((activePlayers & (1U << slot)) != 0) {
            reference += referenceDamage;
            damage += attemptedDamage[slot];
        }
    }
    if (reference <= 0 || !std::isfinite(reference)) {
        return 1 + bonus;
    }
    // Boss e088785c/e08878a4 use authored/current health, not lost-health
    // fraction. Each threshold raises the minimum even after substantial damage
    // inflicted on the party; the frozen level bonus is added after the clamp.
    const f32 ratio = currentHealth > 0 ? authoredHealth / currentHealth : authoredHealth + 1;
    const f32 floor = ratio >= 10 ? 5.0f : ratio >= 4 ? 2.5f : ratio >= 2 ? 1.0f : 0.25f;
    const f32 pressure = ((reference - damage) / reference) * ratio;
    // Preserve the native ordered comparison: a non-finite intermediate cannot
    // escape as a NaN when tiny positive health overflows the ratio.
    return (floor <= pressure ? std::min(pressure, 5.0f) : floor) + bonus;
}
} // namespace gdl::game
