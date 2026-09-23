#pragma once

#include <array>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {
/** The Temple Skorne's four death drops, in the order of its reward table. */
class SkorneRelics {
public:
    struct Drop {
        std::string_view name;
        Vec3 velocity;
    };
    static constexpr f32 kStrength = 240;
    static constexpr f32 kNoGrabSeconds = 2;
    static std::array<Drop, 4> spray(const Vec3& velocity, f32 halfAngle);
};
} // namespace gdl::game
