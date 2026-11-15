#pragma once

#include <string_view>

#include "expansions/shared/characters/LegendsProgress.h"

namespace gdl::game {

/** The ids the settings name the Legends HUD looks by. A saved choice keeps its id, so these
 * never change. */
inline constexpr std::string_view kLegendsN64HudStyle = "n64";
inline constexpr std::string_view kLegendsDreamcastHudStyle = "dreamcast";
/** The text ids of those looks' names on the HUD settings page. */
inline constexpr std::string_view kLegendsN64HudLabel = "hud.style.n64";
inline constexpr std::string_view kLegendsDreamcastHudLabel = "hud.style.dreamcast";

/** The id of the HUD look native to a Legends platform's own game. */
constexpr std::string_view legendsHudStyleOf(LegendsPlatform platform) {
    switch (platform) {
    case LegendsPlatform::N64: return kLegendsN64HudStyle;
    case LegendsPlatform::Dreamcast: return kLegendsDreamcastHudStyle;
    }
    return {};
}

} // namespace gdl::game
