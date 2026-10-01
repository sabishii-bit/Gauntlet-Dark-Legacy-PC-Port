#pragma once

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {
/** mbBlitCalcLight: seven phases of a 150-tick sweep, sampled at each blit's
 * corners. Coordinates are the shop's 512x384 window, with Y rising upward. */
inline u8 shopFrameLight(s32 x, s32 y, f64 seconds) {
    f64 value = (1.0f / 896.0f) * static_cast<f32>(x + y);
    const auto fraction = static_cast<f32>(std::fmod(seconds, 2.5) / 2.5);
    const f64 phase = static_cast<f32>(7.0 * fraction);
    if (value < 0.5) {
        if (phase < 1) {
            value = phase - 4 * value;
        } else if (phase < 2) {
            value = 2 * value * (phase - 3) + 1;
        } else if (phase < 3) {
            value = 2 * (3 - phase) * (0.5 - value) + 2 * (phase - 2) * value;
        } else if (phase < 4) {
            value = 2 * value + 2 * (3 - phase) * (0.5 - value);
        } else if (phase < 5) {
            value = 2 * (3 - phase) * (0.5 - value) + 2 * (5 - phase) * value;
        } else {
            value = 0;
        }
    } else {
        value = 1 - value;
        if (phase < 2) {
            value = 0;
        } else if (phase < 3) {
            value = 2 * (phase - 4) * (0.5 - value) + 2 * (phase - 2) * value;
        } else if (phase < 4) {
            value = 2 * value + 2 * (phase - 4) * (0.5 - value);
        } else if (phase < 5) {
            value = 2 * (phase - 4) * (0.5 - value) + 2 * (5 - phase) * value;
        } else if (phase < 6) {
            value = 2 * value * (4 - phase) + 1;
        } else {
            value = 7 - phase - 4 * value;
        }
    }
    const auto result = static_cast<f32>(static_cast<f64>(static_cast<f32>(value)) * 127);
    return static_cast<u8>(128 + static_cast<s32>(std::clamp(result, 0.0f, 127.0f)));
}
} // namespace gdl::game
