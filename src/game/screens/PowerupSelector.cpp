#include "game/screens/PowerupSelector.h"

#include <algorithm>
#include <cstddef>

namespace gdl::game {

void PowerupSelector::close() {
    m_state = State::Closed;
    m_selection = -1;
    m_slide = 0;
}

SelectorCue PowerupSelector::step(const SelectorInput& input, Inventory& inventory, int ticks) {
    SelectorCue cue = SelectorCue::None;
    const auto held = [&](int slot) {
        return slot >= 0 && static_cast<std::size_t>(slot) < inventory.powerups.size() &&
               inventory.powerups[static_cast<std::size_t>(slot)].held();
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
            PowerupSlot& slot = inventory.powerups[static_cast<std::size_t>(m_selection)];
            slot.on = !slot.on;
            cue = SelectorCue::Switched;
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
