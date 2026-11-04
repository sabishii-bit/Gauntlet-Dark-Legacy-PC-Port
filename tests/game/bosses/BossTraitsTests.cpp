#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/bosses/BossTraits.h"
#include "game/level/BossKinds.h"

namespace {

using namespace gdl;
using namespace gdl::game;

constexpr s32 kLowest = std::numeric_limits<s32>::min();
constexpr std::array<s32, 4> kNoBoss{-1, 33, 45, kLowest};

TEST_CASE("each boss code's traits sit at its own row", "[game][bosses][boss-traits]") {
    for (s32 code = kFirstBossType; code <= kLastBossType; ++code) {
        CAPTURE(code);
        const BossTraits* traits = bossTraitsOf(code);
        REQUIRE(traits != nullptr);
        CHECK(traits->kind == code);
        CHECK(&legendStagingOf(code) == &traits->legend);
    }
}

TEST_CASE("a code outside the bosses has no traits and the default legend staging",
          "[game][bosses][boss-traits]") {
    const LegendStaging defaults{};
    for (const s32 code : kNoBoss) {
        CAPTURE(code);
        CHECK(bossTraitsOf(code) == nullptr);
        CHECK(legendStagingOf(code) == defaults);
    }
}

} // namespace
