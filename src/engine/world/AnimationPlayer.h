#pragma once

#include <cstdint>

#include "engine/assets/AnimationSet.h"

namespace gdl {

/**
 * Plays one sequence of a tree on the game clock the way the original engine steps its
 * animations: a sequence's rate is 900 divided by the frames it shows per second, the frame
 * snaps to whole numbers, a repeating sequence wraps to frame 0 the step it passes its last
 * frame and a one-shot holds that frame. A transition holds the first frame while the caller
 * blends the pose across from whatever showed before.
 */
class AnimationPlayer {
public:
    static constexpr float kRateUnit = 1.0f / 900.0f; ///< seconds per frame per unit of rate
    static constexpr float kDefaultRate = 30.0f;
    static constexpr float kSnapWindow = 0.125f; ///< nearer a whole frame than this snaps to it
    static constexpr float kTick = 1.0f / 30.0f; ///< frames shorter than a tick always snap

    /** Starts `sequence` at `frame`, holding it for `transitionSeconds` first. */
    void start(const TreeSequenceInfo& sequence, std::uint32_t index,
               float transitionSeconds = 0.0f, float frame = 0.0f);
    void stop();

    /** Steps by `seconds`; true the step the sequence wraps or reaches its end. A finished
     * one-shot stays put until started again. */
    bool advance(float seconds, bool repeat);

    bool playing() const { return m_sequence != nullptr; }
    std::uint32_t sequence() const { return m_index; }
    float frame() const { return m_frame; }
    std::int32_t frameCount() const { return m_sequence != nullptr ? m_sequence->frames : 0; }
    /** True from the step the sequence wrapped or ended until the next step moves on. */
    bool finished() const { return m_finished; }
    bool transitioning() const { return m_transitionTime < m_transitionLength; }
    /** How far the transition into this sequence has come, 1 once it is over. */
    float transition() const;

    /** Lets frames fall between whole numbers when a sequence is slower than the clock. */
    void setSmooth(bool smooth) { m_smooth = smooth; }
    /** Scales playback: 2 plays twice as fast. */
    void setSpeed(float speed) { m_speed = speed; }
    float secondsPerFrame() const { return m_secondsPerFrame; }

private:
    const TreeSequenceInfo* m_sequence = nullptr;
    std::uint32_t m_index = 0;
    float m_secondsPerFrame = kDefaultRate * kRateUnit;
    float m_time = 0.0f; ///< seconds into the sequence
    float m_frame = 0.0f;
    float m_speed = 1.0f;
    float m_transitionLength = 0.0f;
    float m_transitionTime = 0.0f;
    bool m_finished = false;
    bool m_held = false; ///< a one-shot that has ended
    bool m_smooth = false;
};

} // namespace gdl
