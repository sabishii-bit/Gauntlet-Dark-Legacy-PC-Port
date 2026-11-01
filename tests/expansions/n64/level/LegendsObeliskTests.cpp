#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "expansions/n64/level/LegendsObelisk.h"

using namespace gdl;
using namespace gdl::game;

TEST_CASE("Native obelisk flashes before changing its authored model at the midpoint",
          "[legends][obelisks][obelisk-motion]") {
    LegendsObeliskMotion motion;
    CHECK_FALSE(motion.started());
    CHECK_FALSE(motion.spentModel());
    CHECK(motion.liveParticles() == 0);
    motion.advance(1);
    REQUIRE(motion.activate({10, 20, 30}, 1));
    REQUIRE_FALSE(motion.activate({40, 50, 60}, 2));
    CHECK(motion.whiteBlend() == 0);
    constexpr std::array<u8, 11> kWeights{0, 51, 102, 153, 204, 255, 204, 153, 102, 51, 0};
    for (usize tick = 0; tick < kWeights.size(); ++tick) {
        CAPTURE(tick);
        motion.advance(1.0f / 30);
        CHECK(motion.whiteBlend() == Catch::Approx(static_cast<f32>(kWeights[tick]) / 255));
        CHECK(motion.spentModel() == (tick >= 5));
    }
    CHECK_FALSE(motion.activate({}, 3));
    motion.advance(1000000);
    CHECK(motion.spentModel());
    CHECK(motion.whiteBlend() == 0);
    CHECK(motion.liveParticles() == 0);
}

TEST_CASE("Native obelisk admission restores quietly and cannot replay its transition",
          "[legends][obelisks][obelisk-motion]") {
    LegendsObeliskMotion motion;
    motion.restoreSpent();
    CHECK(motion.started());
    CHECK(motion.spentModel());
    CHECK_FALSE(motion.activate({}, 1));
    motion.advance(1);
    CHECK(motion.whiteBlend() == 0);
    CHECK(motion.liveParticles() == 0);
}

TEST_CASE("Native obelisk particles retain quantized age motion and finite bounded lifetime",
          "[legends][obelisks][obelisk-particles]") {
    LegendsObeliskMotion motion;
    REQUIRE(motion.activate({}, 1));
    CHECK(motion.emitterPosition() == Vec3(0, 0, 3));
    motion.advance(1.0f / 30);
    REQUIRE(motion.liveParticles() == 7);
    const auto& first = motion.particles().front();
    // Independent e0010c90 seed1 sequence and e0023188/e0023f34 arithmetic.
    CHECK(first.direction == std::array<s8, 3>{4, -113, -56});
    CHECK(first.coordinate == std::array<s32, 3>{0, -8, 188});
    CHECK(first.speed == 3228);
    CHECK(first.age == 29);
    CHECK(first.gravity == 2);
    CHECK(first.alpha == 234);
    CHECK(motion.emitterPosition().x == Catch::Approx(1.5f));
    CHECK(motion.emitterPosition().y == Catch::Approx(.1f));
    CHECK(motion.emitterPosition().z == Catch::Approx(2.598076f));
    for (u32 tick = 1; tick < 120; ++tick) {
        motion.advance(1.0f / 30);
        CHECK(motion.liveParticles() <= LegendsObeliskMotion::kCapacity);
        for (const auto& particle : motion.particles()) {
            if (particle.alive) {
                CHECK(particle.age <= 105);
                CHECK(particle.alpha > 0);
                CHECK(std::isfinite(particle.position().x));
                CHECK(std::isfinite(particle.position().y));
                CHECK(std::isfinite(particle.position().z));
            }
        }
    }
    CHECK(motion.liveParticles() == 0);
}

TEST_CASE("Native obelisk clocks and cosmetic RNG do not depend on rendering frequency",
          "[legends][obelisks][obelisk-motion]") {
    for (const s32 rate : {30, 60, 144}) {
        CAPTURE(rate);
        LegendsObeliskMotion expected;
        LegendsObeliskMotion partitioned;
        REQUIRE(expected.activate({3, 4, 5}, 19));
        REQUIRE(partitioned.activate({3, 4, 5}, 19));
        for (s32 step = 0; step < 30; ++step) {
            expected.advance(1.0f / 30);
        }
        for (s32 step = 0; step < rate; ++step) {
            partitioned.advance(1.0f / static_cast<f32>(rate));
        }
        CHECK(partitioned.whiteBlend() == expected.whiteBlend());
        CHECK(partitioned.spentModel() == expected.spentModel());
        CHECK(partitioned.emitterPosition() == expected.emitterPosition());
        CHECK(std::ranges::equal(partitioned.particles(), expected.particles()));
    }
}

TEST_CASE("Native obelisk ignores non-finite clocks and rejects invalid emitter origins",
          "[legends][obelisks][obelisk-motion]") {
    LegendsObeliskMotion motion;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(motion.activate({nan, 0, 0}, 1));
    CHECK_FALSE(motion.activate({0, infinity, 0}, 1));
    CHECK_FALSE(motion.activate({1.0e30f, 0, 0}, 1));
    REQUIRE(motion.activate({}, 1));
    for (const f32 seconds : {0.0f, -1.0f, nan, infinity}) {
        motion.advance(seconds);
        CHECK(motion.whiteBlend() == 0);
        CHECK(motion.liveParticles() == 0);
        CHECK_FALSE(motion.spentModel());
    }
}

TEST_CASE("Native obelisk sparkle combiner preserves alpha and its borrowed pixels",
          "[legends][obelisks][obelisk-particles]") {
    Image source{3, 1, {0, 0, 0, 0, 255, 255, 255, 255, 128, 64, 32, 73}};
    const auto before = source.pixels;
    const auto converted = legendsObeliskSparkle(source);
    CHECK(converted.width == source.width);
    CHECK(converted.height == source.height);
    CHECK(converted.pixel(0, 0) == Color::rgba(0, 248, 0, 0));
    CHECK(converted.pixel(1, 0) == Color::rgba(248, 248, 248));
    CHECK(converted.pixel(2, 0) == Color::rgba(124, 248, 31, 73));
    CHECK(source.pixels == before);
}
