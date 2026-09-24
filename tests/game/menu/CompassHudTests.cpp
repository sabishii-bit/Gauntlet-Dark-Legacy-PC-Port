#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/menu/CompassHud.h"

namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("compass projects world north through the camera heading", "[compass]") {
    const auto north = CompassHud::directions(0);
    CHECK(north[0] == Vec2{0, -1});
    CHECK(north[1] == Vec2{1, 0});
    const auto east = CompassHud::directions(kPi / 2);
    CHECK(std::abs(east[0].x + 1) < 0.0001f);
    CHECK(std::abs(east[0].y) < 0.0001f);
    CHECK(std::abs(east[1].y + 1) < 0.0001f);
}
TEST_CASE("compass draws localized cardinal labels", "[compass]") {
    auto font = BitmapFont::fromGlyphs(
        10, 4, {{'N', 8, 0, 0}, {'E', 8, 0, 0}, {'S', 8, 0, 0}, {'W', 8, 0, 0}});
    test::FakeTexture texture{64, 64};
    TextPainter text;
    text.setFont(&font, &texture);
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    CompassHud::draw(canvas, text, &strings, 512, 0);
    canvas.end();
    CHECK_FALSE(device.draws.empty());
}
} // namespace
