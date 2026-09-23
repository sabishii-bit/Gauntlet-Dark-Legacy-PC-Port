#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/WorldLighting.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/SumnerFigure.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

TEST_CASE("Sumner stands at his lookout, cycles his idles and gestures on request",
          "[game][world][unpacked]") {
    const std::filesystem::path root = test::unpackedOrSkip("ITEMS/LEVELL/animations.json")
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    test::unpackedOrSkip("LEVELS/LEVELL1/world.json");
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    test::FakeRenderDevice device;
    ItemArchive items;
    REQUIRE(items.load(root / "ITEMS/LEVELL"));
    SumnerFigure sumner;
    REQUIRE_FALSE(sumner.loaded());
    REQUIRE(sumner.load(device, items, layout));
    REQUIRE(sumner.loaded());
    REQUIRE(sumner.position().x == Approx(2.97f).margin(0.01f));
    REQUIRE(sumner.position().z == Approx(-53.47f).margin(0.01f));
    REQUIRE(sumner.yaw() == Approx(-3.0954f + kPi).margin(0.001f)); // turned to the party
    REQUIRE(sumner.sequence() == 0);                                // the stance
    REQUIRE(sumner.index() == 0);
    REQUIRE_FALSE(sumner.gesturing());

    // The stance loops once (ninety frames), and as it wraps the cycle asks for the reading,
    // which starts when the stance next ends.
    for (s32 i = 0; i < 90; ++i) {
        sumner.update(kStep);
    }
    REQUIRE(sumner.index() == 1);
    REQUIRE(sumner.sequence() == 0);
    for (s32 i = 0; i < 90; ++i) {
        sumner.update(kStep);
    }
    REQUIRE(sumner.sequence() == 1);
    REQUIRE(sumner.index() == 2);

    sumner.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE(device.draws.size() > 10);

    // The welcome gesture cuts in at once and the cycle resumes from the stance after it.
    sumner.gesture();
    sumner.update(kStep);
    REQUIRE(sumner.gesturing());
    REQUIRE(sumner.sequence() == 6);
    REQUIRE(sumner.index() == 0);
    s32 steps = 0;
    while (sumner.gesturing() && steps < 130) {
        sumner.update(kStep);
        ++steps;
    }
    REQUIRE_FALSE(sumner.gesturing());
    REQUIRE(sumner.sequence() == 0);
    REQUIRE(steps > 100); // seventy-five frames at twenty a second
    // His greeting and his send-off cut in the same way, by the original's indices.
    sumner.play(SumnerFigure::kWelcomeIndex);
    sumner.update(kStep);
    REQUIRE(sumner.playing(SumnerFigure::kWelcomeIndex));
    REQUIRE(sumner.sequence() == 3);
    sumner.play(SumnerFigure::kGoAwayIndex);
    sumner.update(kStep);
    REQUIRE(sumner.playing(SumnerFigure::kGoAwayIndex));
    REQUIRE_FALSE(sumner.playing(SumnerFigure::kWelcomeIndex));
    REQUIRE_FALSE(sumner.playing(42));

    sumner.clear();
    REQUIRE_FALSE(sumner.loaded());
}

TEST_CASE("Sumner is absent without his item set", "[game][world]") {
    test::FakeRenderDevice device;
    const WorldLayout layout;
    ItemArchive items;
    REQUIRE_FALSE(items.load(test::scratchDirectory("sumner-none")));
    REQUIRE_FALSE(items.loaded());
    SumnerFigure sumner;
    REQUIRE_FALSE(sumner.load(device, items, layout));
    REQUIRE_FALSE(sumner.loaded());
    sumner.gesture();
    sumner.update(kStep);
    sumner.draw(device, Mat4{1.0f}, WorldLighting{});
    REQUIRE(device.draws.empty());
}

} // namespace
