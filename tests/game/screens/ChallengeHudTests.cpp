#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/ChallengeHud.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("the hourglass crops its two sand reservoirs as time elapses", "[secret][hud]") {
    const auto full = ChallengeHud::sand(60, 60);
    CHECK(full.upper.y == 24);
    CHECK(full.upper.height == 41);
    CHECK(full.upperUv.y == Approx(23.0f / 128));
    CHECK(full.lower.y == 106);
    CHECK(full.lower.height == 23);
    const auto empty = ChallengeHud::sand(0, 60);
    CHECK(empty.upper.y == 63);
    CHECK(empty.upper.height == 2);
    CHECK(empty.upperUv.height == 0);
    CHECK(empty.lower.y == 68);
    CHECK(empty.lower.height == 61);
    CHECK(empty.lowerUv.y == Approx(67.0f / 128));
    CHECK(ChallengeHud::sand(-10, 60).lower.height == 61);
    CHECK(ChallengeHud::sand(80, 60).upper.height == 41);
}

TEST_CASE("hourglass uses the shipped sand animation and draws both cropped halves",
          "[secret][hud][unpacked]") {
    const auto directory = test::unpackedOrSkip("POWERUPS/textures.json").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(directory));
    ChallengeHud hud;
    REQUIRE(hud.bind(device, archive));
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    hud.draw(canvas, 30, 60, true);
    canvas.end();
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws.back().vertices.size() == 12);
    const auto* first = device.draws[1].texture;
    hud.step(1.0f / 30);
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    hud.draw(canvas, 29, 60, true);
    canvas.end();
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws[1].texture != first);
    hud.clear();
}
} // namespace
