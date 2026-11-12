#pragma once

#include "engine/core/Types.h"

#include "expansions/n64/camera/LegendsCameraChannel.h"

namespace gdl::test {
/** The update the camera channel and rig cases step by: a thirtieth of a second. */
inline constexpr f32 kCameraUpdate = 1.0f / 30;
/** The Tower's camera times for its shots, e08694e4's. */
inline constexpr game::LegendsCameraChannel::Times kTowerShotTimes{
    .acceleration = .7f, .cruise = 1, .braking = .3f};
/** The Tower's camera times for the return, case 0x28's. */
inline constexpr game::LegendsCameraChannel::Times kTowerReturnTimes{
    .acceleration = .5f, .cruise = 1, .braking = .3f};
} // namespace gdl::test
