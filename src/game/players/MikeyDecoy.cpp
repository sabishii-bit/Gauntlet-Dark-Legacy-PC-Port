#include "game/players/MikeyDecoy.h"

#include <algorithm>
#include <utility>

#include "game/players/PowerupEffects.h"

namespace gdl::game {
void MikeyDecoy::update(f32 seconds, Inventory& inventory, const Vec3& position, f32 timerRate) {
    m_frames += static_cast<f64>(std::max(seconds, 0.0f)) * kFramesPerSecond;
    while (m_frames >= 1) {
        m_frames -= 1;
        step(inventory, position, timerRate);
    }
}

void MikeyDecoy::step(Inventory& inventory, const Vec3& position, f32 timerRate) {
    const auto found = std::ranges::find_if(inventory.powerups, [](const PowerupSlot& slot) {
        return slot.kind == powerup::kSpecial && slot.flags == powerup::kMikey;
    });
    PowerupSlot* slot = found != inventory.powerups.end() ? &*found : nullptr;
    if (slot != nullptr && slot->working() && slot->strength > 0) {
        slot->strength = std::max(0.0f, slot->strength - timerRate / kFramesPerSecond);
    }
    if (m_state == 0) {
        if (slot == nullptr || !slot->working()) {
            return;
        }
        m_position = position;
        m_state = 2;
        ++m_generation;
        slot->on = false;
        return;
    }
    if (m_state == kDespawnFrame) {
        m_state = 0;
        if (slot != nullptr) {
            slot->on = false;
        }
        return;
    }
    if (m_state < kSparkleEnd && m_state % kSparklePeriod == 0) {
        ++m_sparkles;
    }
    ++m_state;
    if (slot != nullptr && slot->working()) {
        m_state = kDespawnFrame;
    }
}

void MikeyDecoy::clear() {
    m_state = 0;
    m_frames = 0;
    m_sparkles = 0;
}

s32 MikeyDecoy::takeSparkles() {
    return std::exchange(m_sparkles, 0);
}
} // namespace gdl::game
