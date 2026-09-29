#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/BodyGlow.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("a glow fades to nine tenths a frame and is gone under a twentieth",
          "[game][players][glow]") {
    constexpr f32 kFrame = 1.0f / 30.0f;
    BodyGlow glow;
    glow.step(kFrame);
    CHECK(glow.level() == 0.0f);
    glow.raise(BodyGlow::kStrike);
    glow.step(kFrame);
    CHECK(glow.level() == Approx(1.8f));
    // Two frames at 60 Hz fade as one at 30.
    BodyGlow fine;
    fine.raise(BodyGlow::kStrike);
    fine.step(kFrame / 2.0f);
    fine.step(kFrame / 2.0f);
    CHECK(fine.level() == Approx(1.8f));
    // 2 x 0.9^n drops under 0.05 at the 36th frame.
    s32 frames = 1;
    while (glow.level() > 0.0f && frames < 100) {
        glow.step(kFrame);
        ++frames;
    }
    CHECK(frames == 36);
}

TEST_CASE("a glow is added to the body's ambient", "[game][players][glow]") {
    WorldLighting level;
    level.ambient = Vec3{0.1f};
    BodyGlow glow;
    CHECK(glow.apply(level).ambient == level.ambient);
    glow.raise(BodyGlow::kOthers);
    CHECK(glow.apply(level).ambient.x == Approx(0.9f));
    CHECK(glow.apply(level).lightColor == level.lightColor);
    // Lit through it, a surface facing away still shows bright.
    const Color dark = level.shade(Vec3{0, -1, 0});
    const Color bright = glow.apply(level).shade(Vec3{0, -1, 0});
    CHECK(bright.r > dark.r);
}

} // namespace
