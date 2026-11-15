#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "expansions/shared/characters/LegendsProgress.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "game/config/GameConfig.h"

namespace {

using namespace gdl;
using namespace gdl::game;

static_assert(legendsHudStyleOf(LegendsPlatform::N64) == kLegendsN64HudStyle);
static_assert(legendsHudStyleOf(LegendsPlatform::Dreamcast) == kLegendsDreamcastHudStyle);

TEST_CASE("each Legends platform names its own HUD look, never Dark Legacy's",
          "[legends][hud-style]") {
    // A saved choice names its look by these ids, and the settings page its name by these keys.
    CHECK(legendsHudStyleOf(LegendsPlatform::N64) == "n64");
    CHECK(legendsHudStyleOf(LegendsPlatform::Dreamcast) == "dreamcast");
    CHECK(kLegendsN64HudLabel == "hud.style.n64");
    CHECK(kLegendsDreamcastHudLabel == "hud.style.dreamcast");
    for (usize index = 0; index < kLegendsPlatformCount; ++index) {
        CAPTURE(index);
        const std::string_view id = legendsHudStyleOf(static_cast<LegendsPlatform>(index));
        CHECK_FALSE(id.empty());
        CHECK(id != kDarkLegacyHudStyle);
    }
    const std::array<std::string_view, 3> ids{kDarkLegacyHudStyle, kLegendsN64HudStyle,
                                              kLegendsDreamcastHudStyle};
    for (usize first = 0; first < ids.size(); ++first) {
        for (usize second = first + 1; second < ids.size(); ++second) {
            CAPTURE(ids.at(first), ids.at(second));
            CHECK(ids.at(first) != ids.at(second));
        }
    }
    CHECK(kLegendsN64HudLabel != kLegendsDreamcastHudLabel);
}

} // namespace
