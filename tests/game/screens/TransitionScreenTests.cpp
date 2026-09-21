#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/TransitionScreen.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Phase = TransitionScreen::Phase;

TEST_CASE("the transition picture comes up over two seconds, covers, and clears",
          "[game][screens][transition]") {
    TransitionScreen screen;
    REQUIRE(screen.phase() == Phase::Off);
    REQUIRE_FALSE(screen.showing());
    screen.clearAway(); // nothing to clear
    REQUIRE(screen.phase() == Phase::Off);
    screen.comeUp();
    REQUIRE(screen.phase() == Phase::ComingUp);
    screen.update(1.0f);
    REQUIRE(screen.opacity() == Approx(0.5f));
    REQUIRE_FALSE(screen.covering());
    screen.update(1.5f);
    REQUIRE(screen.covering());
    REQUIRE(screen.opacity() == 1.0f);
    screen.comeUp(); // already up
    REQUIRE(screen.covering());
    screen.clearAway();
    screen.update(TransitionScreen::kFadeOutSeconds * 0.5f);
    REQUIRE(screen.opacity() == Approx(0.5f));
    screen.update(1.0f);
    REQUIRE(screen.phase() == Phase::Off);
    REQUIRE(screen.opacity() == 0.0f);
    // A level opening under it: up at once.
    screen.cover();
    REQUIRE(screen.covering());
}

TEST_CASE("without its picture the transition covers the view in black, above the boxes",
          "[game][screens][transition]") {
    test::FakeRenderDevice device;
    TransitionScreen screen;
    REQUIRE_FALSE(screen.load(device, test::scratchDirectory("transition-none")));
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    screen.draw(canvas, 512.0f);
    canvas.end();
    REQUIRE(device.draws.empty()); // off, nothing is drawn
    screen.cover();
    canvas.begin(device, Mat4{1.0f});
    screen.draw(canvas, 512.0f);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    REQUIRE(test::minCorner(device.draws[0]) == Vec2{0.0f, 0.0f});
    REQUIRE(test::maxCorner(device.draws[0]) == Vec2{512.0f, TransitionScreen::kViewHeight});
    REQUIRE(device.draws[0].vertices[0].color.a == 255);
}

TEST_CASE("the unpacked static archive holds the transition picture",
          "[game][screens][transition][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("STATIC/textures.json").parent_path().parent_path();
    test::FakeRenderDevice device;
    TransitionScreen screen;
    REQUIRE(screen.load(device, root));
    screen.release();
}

} // namespace
