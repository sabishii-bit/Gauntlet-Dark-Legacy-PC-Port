#pragma once

#include <array>
#include <string_view>

#include "engine/world/ThermalMaterial.h"

namespace gdl::game {

/** The particle textures that are flames: torches, embers and pool fires (blue ones too),
 * the dragon's breath, the fireballs and the burning weapons' trails. */
inline constexpr auto kThermalParticles = std::to_array<std::string_view>(
    {"P_TORCH", "P_TORCH_P", "NU_TORCH", "EMBER_SPARK2", "POOLFIRE", "POOLFIRE2", "POOLFIREC",
     "BLUEP_TORCH", "BLUEEMBER_SPARK2", "BLUEPOOLFIRE", "DRAGONBREATH", "FBALLX", "FBALLX2",
     "AXEPARTFIRE", "ARCPARTFIRE", "SWORDPARTFIRE"});

/** The animated surface slots that burn, not the masonry or wood beneath them. Lava oceans
 * and distant fire-cloud backdrops are left out: they need a surface mask, not one huge
 * ellipse. */
inline constexpr auto kThermalSurfaces =
    std::to_array<std::string_view>({"TORCHA", "TORCHB", "TORCHC", "NU_TORCH", "FIRE", "FLAME",
                                     "FLAME2_", "FIRE_BOWL", "H_FIREWOOD", "H_FIREBACK"});

/** What shimmers with heat in the levels, for the world scene and every particle field. */
inline constexpr ThermalMaterial kThermalTextures{kThermalParticles, kThermalSurfaces};

} // namespace gdl::game
