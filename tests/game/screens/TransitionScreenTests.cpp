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
    canvas.begin(device, makeScreenProjection(512, 384));
    screen.draw(canvas, 512.0f);
    canvas.end();
    REQUIRE(device.draws.empty()); // off, nothing is drawn
    screen.cover();
    canvas.begin(device, makeScreenProjection(512, 384));
    screen.draw(canvas, 512.0f);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    REQUIRE(test::minCorner(device.draws[0]) == Vec2{0.0f, 0.0f});
    REQUIRE(test::maxCorner(device.draws[0]) == Vec2{512.0f, TransitionScreen::kViewHeight});
    REQUIRE(device.draws[0].vertices[0].color.a == 255);
}

TEST_CASE("releasing or reopening a transition discards every previous animation phase",
          "[game][screens][transition]") {
    test::FakeRenderDevice device;
    TransitionScreen screen;
    SECTION("partially rising") {
        screen.comeUp();
        screen.update(TransitionScreen::kFadeInSeconds / 2);
    }
    SECTION("fully covered") {
        screen.cover();
    }
    SECTION("partially clearing") {
        screen.cover();
        screen.clearAway();
        screen.update(TransitionScreen::kFadeOutSeconds / 2);
    }
    REQUIRE(screen.showing());
    screen.release();
    CHECK(screen.phase() == Phase::Off);
    CHECK(screen.opacity() == 0);
    screen.update(10);
    CHECK_FALSE(screen.showing());

    // A missing archive still begins a fresh transition, not a permanent black cover.
    screen.cover();
    REQUIRE_FALSE(screen.load(device, test::scratchDirectory("transition-reopen-none")));
    CHECK(screen.phase() == Phase::Off);
    CHECK(screen.opacity() == 0);
    Canvas canvas;
    canvas.begin(device, makeScreenProjection(512, 384));
    screen.draw(canvas, 512);
    canvas.end();
    CHECK(device.draws.empty());
}

TEST_CASE("the native static archive holds the transition picture",
          "[game][screens][transition][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    TransitionScreen screen;
    REQUIRE(screen.load(device, root));
    screen.release();
}

TEST_CASE("transition margins follow the picture fade without covering the status boxes",
          "[game][screens][transition]") {
    test::FakeRenderDevice device;
    TransitionScreen screen;
    Canvas canvas;
    const Mat4 transform = makeLetterboxProjection(512, 384, 1920, 1080);
    const auto check = [&](u8 alpha) {
        device.draws.clear();
        canvas.begin(device, transform);
        screen.draw(canvas, 512);
        canvas.end();
        REQUIRE(device.draws.size() == 2);
        CHECK(test::maxCorner(device.draws.front()) == Vec2{512, 320});
        CHECK(device.draws.front().vertices.front().color.a == alpha);
        const auto& margins = device.draws.back();
        CHECK(margins.transform == Mat4{1});
        CHECK_FALSE(margins.state.depthTest);
        CHECK_FALSE(margins.state.depthWrite);
        for (const auto& vertex : margins.vertices) {
            CHECK(vertex.color == Color::black().withAlpha(alpha));
        }
        // Neither margin reaches into the original canvas, including its HUD.
        for (usize i = 0; i < margins.vertices.size(); i += 3) {
            const f32 x = (margins.vertices[i].position.x + margins.vertices[i + 1].position.x +
                           margins.vertices[i + 2].position.x) /
                          3;
            CHECK((x < -0.75f || x > 0.75f));
        }
    };
    screen.comeUp();
    screen.update(TransitionScreen::kFadeInSeconds / 2);
    check(127);
    screen.cover();
    check(255);
    screen.clearAway();
    screen.update(TransitionScreen::kFadeOutSeconds / 2);
    check(127);
    screen.update(TransitionScreen::kFadeOutSeconds);
    device.draws.clear();
    canvas.begin(device, transform);
    screen.draw(canvas, 512);
    canvas.end();
    CHECK(device.draws.empty());
}

} // namespace
