#pragma once

#include "engine/core/Types.h"

namespace gdl {

/**
 * The way the original darkens a level for a moment, as it does while a great move comes out:
 * whatever wants the dark asks for it every tick it lasts, by how much to take off the
 * ambient light; the light falls towards that a quarter a frame, and once nobody asks any
 * more what was asked for fades away (to three fifths each frame) and the light climbs back
 * a twentieth a frame.
 */
class AmbientDimmer {
public:
    static constexpr f32 kFrameSeconds = 1.0f / 30.0f; ///< the original's frame, which paces it
    static constexpr f32 kHoldSeconds = 2.0f / 30.0f;  ///< how long one asking lasts
    static constexpr f32 kDarkenStep = 0.25f;
    static constexpr f32 kBrightenStep = 0.05f;
    static constexpr f32 kDecay = 0.6f;

    /** Asks for `amount` (negative darkens) to be added to the ambient light for now. */
    void ask(f32 amount);
    void update(f32 seconds);
    void reset();

    /** What is added to the ambient light at the moment. */
    f32 offset() const { return m_current; }
    /** An ambient level with it applied, kept between none and full. */
    f32 applied(f32 ambient) const;

private:
    f32 m_wanted = 0.0f;
    f32 m_current = 0.0f;
    f32 m_holdLeft = 0.0f;
    f32 m_frames = 0.0f;
};

} // namespace gdl
