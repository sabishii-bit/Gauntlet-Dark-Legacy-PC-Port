#include "game/players/Inventory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace gdl::game {

std::int32_t Inventory::addKeys(std::int32_t count) {
    const std::int32_t taken = std::clamp(count, 0, kMostKeys - std::min(keys, kMostKeys));
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

std::int32_t Inventory::takePotion() {
    if (potions.empty()) {
        return 0;
    }
    const std::int32_t kind = potions.back();
    potions.pop_back();
    return kind;
}

std::int32_t Inventory::addPotions(std::int32_t kind, std::int32_t count) {
    std::int32_t taken = 0;
    while (taken < count && static_cast<std::int32_t>(potions.size()) < kMostPotions) {
        potions.push_back(kind);
        ++taken;
    }
    return taken;
}

void Inventory::addPowerup(std::int32_t kind, std::uint32_t flags, float charge, float strength) {
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
    constexpr float kOtherForGood = -2.0f;
    constexpr float kSameForGood = -1.0f;
    float best = kOtherForGood;
    std::size_t pick = 0;
    for (std::size_t i = 0; i < powerups.size(); ++i) {
        float weight = powerups[i].strength;
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

const PowerupSlot* Inventory::powerup(std::int32_t kind, std::uint32_t mask) const {
    for (const PowerupSlot& slot : powerups) {
        if (slot.working() && slot.kind == kind && (slot.flags & mask) != 0) {
            return &slot;
        }
    }
    return nullptr;
}

std::int32_t Inventory::nextHeld(std::int32_t from, std::int32_t step) const {
    const auto count = static_cast<std::int32_t>(powerups.size());
    std::int32_t at = from;
    for (std::int32_t tries = 0; tries < count; ++tries) {
        at += step;
        if (at < 0) {
            at = count - 1;
        } else if (at >= count) {
            at = 0;
        }
        if (powerups[static_cast<std::size_t>(at)].held()) {
            return at;
        }
    }
    return -1;
}

std::size_t Inventory::powerupCount() const {
    return static_cast<std::size_t>(
        std::ranges::count_if(powerups, [](const PowerupSlot& slot) { return slot.held(); }));
}

} // namespace gdl::game
