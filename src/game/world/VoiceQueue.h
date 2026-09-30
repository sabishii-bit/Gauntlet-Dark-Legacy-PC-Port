#pragma once

#include <vector>

#include "engine/assets/SoundSet.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

namespace gdl::game {

/**
 * One of the original's two sound queues (sndFxQueAddEx's modes: the narrator's lines, and
 * the characters' cries): lines play one after another, at most sixteen waiting. A line asks
 * for room first, giving how long it is willing to wait behind what is already queued, and
 * is turned away whole when that is longer. The output is borrowed and may be null.
 */
class VoiceQueue {
public:
    static constexpr usize kMost = 16;
    static constexpr f32 kAlwaysRoom = -1.0f; ///< a wait that is never refused

    void bind(SoundPlayer* output) { m_output = output; }
    /** Whether a line willing to wait `maxWait` seconds may be queued now. */
    bool room(f32 maxWait) const;
    /** Queues a sequence behind the rest at `volume`; kNoSound when held or full. */
    SoundHandle queue(const SoundSequence& sequence, f32 volume);
    /** While held nothing more is let in: the good wizard has the floor. */
    void hold(bool held) { m_held = held; }
    bool held() const { return m_held; }
    /** Advances the clock, letting finished lines go. */
    void update(f32 seconds);
    /** Seconds of queued lines still to come. */
    f64 backlog() const;
    /** Forgets what was queued without stopping it. */
    void clear();
    /** How long a sequence plays, in seconds. */
    static f64 lengthOf(const SoundSequence& sequence);

private:
    SoundPlayer* m_output = nullptr;
    f64 m_clock = 0.0;
    std::vector<f64> m_ends; ///< when each queued line not yet over will end
    SoundHandle m_tail = kNoSound;
    bool m_held = false;
};

} // namespace gdl::game
