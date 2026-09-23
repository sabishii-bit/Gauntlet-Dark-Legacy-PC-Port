#include "game/players/Inventory.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl::game {

s32 Inventory::addKeys(s32 count) {
    const s32 taken = std::clamp(count, 0, kMostKeys - std::min(keys, kMostKeys));
    keys += taken;
    return taken;
}

bool Inventory::spendKey() {
    if (keys <= 0) {
        return false;
    }
    --keys;
    return true;
}

s32 Inventory::takePotion() {
    if (potions.empty()) {
        return 0;
    }
    const s32 kind = potions.back();
    potions.pop_back();
    return kind;
}

s32 Inventory::addPotions(s32 kind, s32 count) {
    s32 taken = 0;
    while (taken < count && static_cast<s32>(potions.size()) < kMostPotions) {
        potions.push_back(kind);
        ++taken;
    }
    return taken;
}

void Inventory::addPowerup(s32 kind, u32 flags, f32 charge, f32 strength) {
    for (PowerupSlot& slot : powerups) {
        if (slot.kind != kind || slot.flags != flags) {
            continue;
        }
        if (charge > 0.0f) {
            slot.charge += charge;
        }
        if (slot.strength >= 0.0f && strength > 0.0f) {
            slot.strength += kRenewShare * strength;
        } else if (strength < 0.0f) {
            slot.strength = strength;
        }
        return;
    }
    // The first free slot, else the one with least left; one held for good gives way only
    // when nothing else does, its own kind last of all.
    constexpr f32 kOtherForGood = -2.0f;
    constexpr f32 kSameForGood = -1.0f;
    f32 best = kOtherForGood;
    usize pick = 0;
    for (usize i = 0; i < powerups.size(); ++i) {
        f32 weight = powerups[i].strength;
        if (weight < 0.0f) {
            weight = powerups[i].kind == kind ? kSameForGood : kOtherForGood;
        }
        if (best == kOtherForGood || weight == 0.0f || (weight >= 0.0f && weight < best)) {
            best = weight;
            pick = i;
        }
        if (best == 0.0f) {
            break;
        }
    }
    powerups[pick] = PowerupSlot{strength, kind, charge, flags, true};
}

const PowerupSlot* Inventory::powerup(s32 kind, u32 mask) const {
    for (const PowerupSlot& slot : powerups) {
        if (slot.working() && slot.kind == kind && (slot.flags & mask) != 0) {
            return &slot;
        }
    }
    return nullptr;
}

s32 Inventory::nextHeld(s32 from, s32 step) const {
    const auto count = static_cast<s32>(powerups.size());
    s32 at = from;
    for (s32 tries = 0; tries < count; ++tries) {
        at += step;
        if (at < 0) {
            at = count - 1;
        } else if (at >= count) {
            at = 0;
        }
        if (powerups[static_cast<usize>(at)].held()) {
            return at;
        }
    }
    return -1;
}

usize Inventory::powerupCount() const {
    return static_cast<usize>(
        std::ranges::count_if(powerups, [](const PowerupSlot& slot) { return slot.held(); }));
}

} // namespace gdl::game
