#pragma once

#include <cstdint>

#include "game/players/Inventory.h"

namespace gdl::game {

/** The four presses that work a player's selector, each true the frame it is pressed. */
struct SelectorInput {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
};

/** What a step of the selector did, for its sounds. */
enum class SelectorCue : std::uint8_t { None, Opened, Closed, Moved, Switched };

/**
 * The powerup selector over a player's status box, worked the way the original's is: up
 * slides a label up naming one of the powerups carried; left and right go round the others;
 * up again takes the named one off or puts it back on; down slides the label away. A
 * powerup that runs out while named hands over to the one before it, and the last one
 * running out closes the selector.
 */
class PowerupSelector {
public:
    enum class State : std::uint8_t { Closed, SlidingIn, Open, SlidingOut };
    static constexpr std::int32_t kSlide = 128; ///< how far the label rides up
    static constexpr std::int32_t kSlidePerTick = 4;
    static constexpr std::int32_t kLabelX = 12;    ///< from the box's left
    static constexpr std::int32_t kLabelRise = 25; ///< its resting height over the box's top
    static constexpr float kLabelScale = 0.45f;

    SelectorCue step(const SelectorInput& input, Inventory& inventory, std::int32_t ticks);
    void close();

    State state() const { return m_state; }
    bool showing() const { return m_state == State::Open; }
    /** The slot named, or -1. */
    std::int32_t selection() const { return m_selection; }
    std::int32_t slide() const { return m_slide; }
    /** The label's top on a box whose top is `boxY`. */
    std::int32_t labelY(std::int32_t boxY) const { return boxY - kLabelRise + kSlide - m_slide; }

private:
    State m_state = State::Closed;
    std::int32_t m_selection = -1;
    std::int32_t m_slide = 0;
};

} // namespace gdl::game
