#include <catch2/catch_test_macros.hpp>

#include "expansions/dreamcast/camera/LegendsDreamcastCameraBounds.h"

namespace gdl::game {
namespace {
TEST_CASE("Dreamcast camera initialization derives only unauthored limits from world bounds",
          "[legends][dreamcast-camera]") {
    LevelCameraInfo source;
    source.authoredBounds = false;
    source.radiusMin = 20;
    source.radiusMax = 32;
    const auto derived = resolveDreamcastCameraBounds(source, {-100, -30, -80}, {90, 50, 120});
    CHECK(derived.boundsMin == Vec3{-92, -30, -72});
    CHECK(derived.boundsMax == Vec3{82, 54, 112});
    CHECK(derived.radiusMin == source.radiusMin);
    CHECK(derived.radiusMax == source.radiusMax);
    source.authoredBounds = true;
    source.boundsMin = {-10, -20, -30};
    source.boundsMax = {40, 50, 60};
    const auto authored = resolveDreamcastCameraBounds(source, {-100, -30, -80}, {90, 50, 120});
    CHECK(authored.boundsMin == source.boundsMin);
    CHECK(authored.boundsMax == source.boundsMax);
}
} // namespace
} // namespace gdl::game
