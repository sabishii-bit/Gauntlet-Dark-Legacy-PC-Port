#include "game/hud/PowerupSelector.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "engine/core/Types.h"

#include "game/players/PowerupEffects.h"

namespace gdl::game {

f32 PowerupSelector::labelScale(const TextPainter& text, std::string_view label) {
    const s32 full = text.measure(label, kLabelScale);
    if (full <= kLabelBudget) {
        return kLabelScale;
    }
    // Glyph advances are truncated to whole pixels, so a single proportion can
    // still stick out. The lower bound of this search is always a size that fits.
    f32 low = 0;
    f32 high = kLabelScale;
    for (s32 attempt = 0; attempt < 16; ++attempt) {
        const f32 mid = (low + high) * 0.5f;
        if (text.measure(label, mid) <= kLabelBudget) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return low;
}

void PowerupSelector::close() {
    m_state = State::Closed;
    m_selection = -1;
    m_slide = 0;
    m_acquired.clear();
}

void PowerupSelector::focus(const Inventory& inventory, s32 kind, u32 flags) {
    const auto index =
        std::distance(inventory.powerups.begin(),
                      std::ranges::find_if(inventory.powerups, [&](const PowerupSlot& slot) {
                          return slot.held() && slot.kind == kind && slot.flags == flags;
                      }));
    if (index == std::ssize(inventory.powerups)) {
        return;
    }
    m_selection = static_cast<s32>(index);
    std::erase(m_acquired, m_selection);
    m_acquired.insert(m_acquired.begin(), m_selection);
}

bool PowerupSelector::charged(const PowerupSlot& slot) {
    return (slot.kind == powerup::kWeapon &&
            (slot.flags & (powerup::kSuperShot | powerup::kThunderHammer)) != 0) ||
           (slot.kind == powerup::kSpecial && (slot.flags & powerup::kBreath) != 0);
}

std::optional<f32> PowerupSelector::remaining(const PowerupSlot& slot) {
    if (!slot.working()) {
        return std::nullopt;
    }
    const f32 value = charged(slot) ? slot.charge : slot.strength;
    if (!std::isfinite(value) || value < 0 || (charged(slot) && value < 1)) {
        return std::nullopt;
    }
    return charged(slot) ? std::floor(value) : value;
}

s32 PowerupSelector::usageSlot(const Inventory& inventory) const {
    const auto usable = [&](s32 slot) {
        return slot >= 0 && static_cast<usize>(slot) < inventory.powerups.size() &&
               remaining(inventory.powerups[static_cast<usize>(slot)]).has_value();
    };
    if (usable(m_selection)) {
        return m_selection;
    }
    for (const s32 slot : m_acquired) {
        if (usable(slot)) {
            return slot;
        }
    }
    for (s32 slot = static_cast<s32>(inventory.powerups.size()) - 1; slot >= 0; --slot) {
        if (usable(slot)) {
            return slot;
        }
    }
    return -1;
}

SelectorCue PowerupSelector::step(const SelectorInput& input, Inventory& inventory, s32 ticks,
                                  bool challenge) {
    SelectorCue cue = SelectorCue::None;
    const auto held = [&](s32 slot) {
        return slot >= 0 && static_cast<usize>(slot) < inventory.powerups.size() &&
               inventory.powerups[static_cast<usize>(slot)].held();
    };
    if (m_state == State::Open) {
        // The one named running out, or left, hands over to the one before it.
        if (!held(m_selection) || input.left) {
            cue = SelectorCue::Moved;
            m_selection = inventory.nextHeld(m_selection, -1);
        } else if (input.right) {
            cue = SelectorCue::Moved;
            m_selection = inventory.nextHeld(m_selection, 1);
        } else if (input.up) {
            PowerupSlot& slot = inventory.powerups[static_cast<usize>(m_selection)];
            if (slot.on || !challenge || powerupAllowedInChallenge(slot.kind, slot.flags)) {
                slot.on = !slot.on;
                cue = SelectorCue::Switched;
            }
        }
        if (m_selection < 0 || input.down) {
            m_state = State::SlidingOut;
            m_slide = 0;
            cue = SelectorCue::Closed;
        }
    } else if (m_state == State::Closed && input.up) {
        if (!held(m_selection)) {
            m_selection = inventory.nextHeld(m_selection, -1);
        }
        if (m_selection >= 0) {
            m_state = State::SlidingIn;
            m_slide = 0;
            cue = SelectorCue::Opened;
        }
    }
    if (m_state == State::SlidingIn || m_state == State::SlidingOut) {
        if (m_slide < kSlide) {
            m_slide = std::min(m_slide + ticks * kSlidePerTick, kSlide);
        } else {
            m_state = m_state == State::SlidingIn ? State::Open : State::Closed;
        }
    }
    return cue;
}

} // namespace gdl::game
