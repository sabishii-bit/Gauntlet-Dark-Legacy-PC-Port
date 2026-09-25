#pragma once

#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** One player's in-level transporter fade. It cannot restart until they leave the pad. */
class PlayerTransport {
public:
    bool begin(const Vec3& destination);
    /** Sixtieths elapsed; emits the relocation exactly once, at the invisible midpoint. */
    std::optional<Vec3> update(s32 ticks);
    void observeContact(bool onPad);
    /** Abort the fade but still require stepping off before another attempt. */
    void cancel() { m_timer = 0; }
    void clear();
    bool active() const { return m_timer > 0; }
    bool armed() const { return !active() && !m_needsStepOff; }
    f32 alpha() const;

private:
    Vec3 m_destination{0.0f};
    s32 m_timer = 0;
    bool m_needsStepOff = false;
};

} // namespace gdl::game
