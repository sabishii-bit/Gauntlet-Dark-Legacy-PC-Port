#pragma once

#include "engine/core/Types.h"

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
enum class SelectorCue : u8 { None, Opened, Closed, Moved, Switched };

/**
 * The powerup selector over a player's status box, worked the way the original's is: up
 * slides a label up naming one of the powerups carried; left and right go round the others;
 * up again takes the named one off or puts it back on; down slides the label away. A
 * powerup that runs out while named hands over to the one before it, and the last one
 * running out closes the selector.
 */
class PowerupSelector {
public:
    enum class State : u8 { Closed, SlidingIn, Open, SlidingOut };
    static constexpr s32 kSlide = 128;        ///< how far the label rides up
    static constexpr s32 kSlidePerTick = 4;
    static constexpr s32 kLabelX = 12;        ///< from the box's left
    static constexpr s32 kLabelRise = 25;     ///< its resting height over the box's top
    static constexpr f32 kLabelScale = 0.45f;

    SelectorCue step(const SelectorInput& input, Inventory& inventory, s32 ticks);
    void close();

    State state() const { return m_state; }
    bool showing() const { return m_state == State::Open; }
    /** The slot named, or -1. */
    s32 selection() const { return m_selection; }
    s32 slide() const { return m_slide; }
    /** The label's top on a box whose top is `boxY`. */
    s32 labelY(s32 boxY) const { return boxY - kLabelRise + kSlide - m_slide; }

private:
    State m_state = State::Closed;
    s32 m_selection = -1;
    s32 m_slide = 0;
};

} // namespace gdl::game
