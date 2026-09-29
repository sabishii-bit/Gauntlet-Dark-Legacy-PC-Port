#pragma once

#include "engine/core/Types.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

/**
 * How far a character shines beyond the level's light (pulse_7FC, PlayerProcessScale): set to
 * two through a strike that darkens the level (PlyrSfxDoDamage) and for the legend's bearer
 * through its rite, to eight tenths for the rest of the party then; it fades to nine tenths a
 * 30 Hz frame and is gone under a twentieth. It is added to the body's ambient, a unit being
 * the whole of a channel (MBTreeSetAmbientAdd of 255 a unit).
 */
class BodyGlow {
public:
    static constexpr f32 kStrike = 2.0f; ///< 0x80347DAC
    static constexpr f32 kBearer = 2.0f;
    static constexpr f32 kOthers = 0.8f;
    static constexpr f32 kFade = 0.9f; ///< 0x803478E8, a 30 Hz frame
    static constexpr f32 kGone = 0.05f;
    static constexpr f32 kFrameRate = 30.0f;

    void raise(f32 level) { m_level = level; }
    /** Fades by `seconds` of 30 Hz frames. */
    void step(f32 seconds);
    f32 level() const { return m_level; }
    /** `lighting` with the glow added to its ambient. */
    WorldLighting apply(const WorldLighting& lighting) const;

private:
    f32 m_level = 0.0f;
};

} // namespace gdl::game
