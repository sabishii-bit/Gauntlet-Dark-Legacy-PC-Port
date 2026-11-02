#include "game/players/MikeyDecoy.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "game/combat/PowerupFlags.h"

namespace gdl::game {
bool MikeyDecoy::Checkpoint::valid() const {
    const auto finite = [](f32 value) {
        return std::isfinite(value) && std::abs(value) <= 1'000'000;
    };
    return finite(position.x) && finite(position.y) && finite(position.z) &&
           std::isfinite(frames) && frames >= 0 && frames < 1 &&
           (state == 0 || (state >= 2 && state <= kDespawnFrame)) && sparkles >= 0 &&
           sparkles <= 65535;
}
bool MikeyDecoy::restore(const Checkpoint& value) {
    if (!value.valid()) {
        return false;
    }
    m_position = value.position;
    m_frames = value.frames;
    m_state = value.state;
    m_sparkles = value.sparkles;
    m_generation = value.generation;
    return true;
}
void MikeyDecoy::update(f32 seconds, Inventory& inventory, const Vec3& position, f32 timerRate) {
    m_frames += static_cast<f64>(std::max(seconds, 0.0f)) * kFramesPerSecond;
    while (m_frames >= 1) {
        m_frames -= 1;
        step(inventory, position, timerRate);
    }
}

void MikeyDecoy::step(Inventory& inventory, const Vec3& position, f32 timerRate) {
    PowerupSlot* slot = nullptr;
    for (auto& candidate : inventory.powerups) {
        if (candidate.kind == powerup::kSpecial && candidate.flags == powerup::kMikey) {
            slot = &candidate;
            break;
        }
    }
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
