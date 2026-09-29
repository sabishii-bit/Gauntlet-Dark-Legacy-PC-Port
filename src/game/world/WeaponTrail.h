#pragma once

#include <array>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/**
 * The ghosts a heavy swing leaves of the weapon (PlayerDoWeapTrail, psfx.c 146): while the
 * slow swing, the spin or the middle power swing plays, each frame leaves a copy of the held
 * weapon where it is, unless it has moved less than a tenth since the last; every copy fades
 * by 32 of 255 a tick and is gone when clear, eight at most, the faintest giving way.
 */
class WeaponTrail {
public:
    static constexpr usize kMost = 8;
    static constexpr s32 kFadePerTick = 32; ///< lbl_80343DB0
    static constexpr s32 kClear = 255;
    static constexpr f32 kLeastStep = 0.1f; ///< how far the weapon must move for another copy

    struct Ghost {
        Mat4 placement{1.0f};
        s32 fade = 0; ///< of 255: nought is solid
        bool shown = false;
        f32 alpha() const { return 1.0f - static_cast<f32>(fade) / static_cast<f32>(kClear); }
    };

    /** Fades the copies by `ticks`, then leaves one at `weapon` while `swinging`. */
    void step(s32 ticks, const Mat4& weapon, bool swinging);
    void clear() { m_ghosts = {}; }
    const std::array<Ghost, kMost>& ghosts() const { return m_ghosts; }
    usize count() const;

private:
    std::array<Ghost, kMost> m_ghosts{};
};

} // namespace gdl::game
