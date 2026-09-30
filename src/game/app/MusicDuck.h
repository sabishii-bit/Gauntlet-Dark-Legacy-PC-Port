#pragma once

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * The music's level under the options menu, as the original runs it: asked to duck, the
 * level falls to nothing a step a 30 Hz frame (AudioClampMusicVol's scale of nought,
 * AudioMusicVolUpdate's step of 8 in 255), and released it climbs back the same way.
 */
class MusicDuck {
public:
    static constexpr f32 kStep = 8.0f / 255.0f; ///< the level's change a frame
    static constexpr f64 kFrameSeconds = 1.0 / 30.0;

    /** Advances the level over `seconds` towards silence when `ducked`, else towards full. */
    void update(f64 seconds, bool ducked);
    /** The music's level, from nought to one. */
    f32 level() const { return m_level; }

private:
    f32 m_level = 1.0f;
    f64 m_remainder = 0.0; ///< the part of a frame not yet stepped
};

} // namespace gdl::game
