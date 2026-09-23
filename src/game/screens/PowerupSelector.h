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
    static constexpr int kSlide = 128; ///< how far the label rides up
    static constexpr int kSlidePerTick = 4;
    static constexpr int kLabelX = 12;    ///< from the box's left
    static constexpr int kLabelRise = 25; ///< its resting height over the box's top
    static constexpr float kLabelScale = 0.45f;

    SelectorCue step(const SelectorInput& input, Inventory& inventory, int ticks);
    void close();

    State state() const { return m_state; }
    bool showing() const { return m_state == State::Open; }
    /** The slot named, or -1. */
    int selection() const { return m_selection; }
    int slide() const { return m_slide; }
    /** The label's top on a box whose top is `boxY`. */
    int labelY(int boxY) const { return boxY - kLabelRise + kSlide - m_slide; }

private:
    State m_state = State::Closed;
    int m_selection = -1;
    int m_slide = 0;
};

} // namespace gdl::game
