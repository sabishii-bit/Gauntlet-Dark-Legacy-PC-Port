#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace gdl {

/** Explicit flame bindings for optional heat distortion; colour and additive blending
 * alone cannot distinguish fire from poison, lightning, portals or reflected light. */
struct ThermalMaterial {
    static bool particle(std::string_view name) {
        constexpr std::array kNames{
            "P_TORCH",          "P_TORCH_P",    "NU_TORCH",     "EMBER_SPARK2",
            "POOLFIRE",         "POOLFIRE2",    "POOLFIREC",    "BLUEP_TORCH",
            "BLUEEMBER_SPARK2", "BLUEPOOLFIRE", "DRAGONBREATH", "FBALLX",
            "FBALLX2",          "AXEPARTFIRE",  "ARCPARTFIRE",  "SWORDPARTFIRE"};
        return std::ranges::find(kNames, name) != kNames.end();
    }

    static bool surface(std::string_view name) {
        // Animated surface slots, not the masonry/wood supporting them. Lava oceans
        // and distant fire-cloud backdrops need a surface mask, not one huge ellipse.
        constexpr std::array kNames{"TORCHA", "TORCHB",  "TORCHC",    "NU_TORCH",   "FIRE",
                                    "FLAME",  "FLAME2_", "FIRE_BOWL", "H_FIREWOOD", "H_FIREBACK"};
        return std::ranges::find(kNames, name) != kNames.end();
    }
};
} // namespace gdl
