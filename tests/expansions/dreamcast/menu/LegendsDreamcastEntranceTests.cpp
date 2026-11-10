#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/io/File.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/menu/LegendsDreamcastEntrance.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "fonts/font32.fnt")) {
        SKIP("Native Dreamcast menu assets are not installed");
    }
    return root;
}
} // namespace

TEST_CASE("Dreamcast entry title retains native timing", "[dreamcast-dialogue]") {
    CHECK(LegendsDreamcastEntrance::scale(-1) == 0);
    CHECK(LegendsDreamcastEntrance::scale(0) == Catch::Approx(.025f));
    CHECK(LegendsDreamcastEntrance::scale(1) == Catch::Approx(.05f));
    CHECK(LegendsDreamcastEntrance::scale(40) == Catch::Approx(1.025f));
    CHECK(LegendsDreamcastEntrance::scale(79) == 2);
    CHECK(LegendsDreamcastEntrance::scale(90) == 2);
    CHECK(LegendsDreamcastEntrance::scale(91) == 0);
    CHECK(LegendsDreamcastEntrance::scale(100000) == 0);
}

TEST_CASE("Dreamcast entrance display names come from LEVL instead of archive names",
          "[dreamcast-dialogue][assets]") {
    const auto root = nativeRoot();
    const auto file = root / formats::dreamcastLegendsWorldDataFile("levelt1");
    if (!std::filesystem::is_regular_file(file)) {
        SKIP("Native Dreamcast world metadata is not installed");
    }
    const auto scope = formats::dreamcastLegendsFixtureScope(readFile(file), "levelt1");
    REQUIRE(scope);
    CHECK(scope->title == "Tower");
}

TEST_CASE("Dreamcast entry title grows on the shared title geometry until its ticks run out",
          "[dreamcast-dialogue][dreamcast-entrance]") {
    // 8c078920 draws LEVL's name centred at 48 - trunc(16 * scale) and stops
    // with the 91-tick hold; a ten-row glyph shows nine rows at each scale.
    const BitmapFont font = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}});
    const test::FakeTexture sheet{32, 16};
    TextPainter text;
    text.setFont(&font, &sheet);
    test::FakeRenderDevice device;
    Canvas canvas;
    const auto draw = [&](f32 width, s32 ticks) {
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        LegendsDreamcastEntrance::draw(canvas, text, "A", width, ticks);
        canvas.end();
    };
    struct Expected {
        s32 ticks;
        f32 top;
        f32 rows;
    };
    for (const auto& expected : {Expected{0, 48.0125f, 9 * .025f}, Expected{39, 32.5f, 9},
                                 Expected{79, 17, 18}, Expected{90, 17, 18}}) {
        CAPTURE(expected.ticks);
        draw(512, expected.ticks);
        REQUIRE(device.draws.size() == 1);
        const Vec2 low = test::minCorner(device.draws[0]);
        const Vec2 high = test::maxCorner(device.draws[0]);
        CHECK(low.y == Catch::Approx(expected.top));
        CHECK(high.y - low.y == Catch::Approx(expected.rows));
        // The painter centres the measured line on whole canvas units.
        CHECK((low.x + high.x) / 2 == Catch::Approx(256).margin(.5));
    }
    draw(640, 39);
    REQUIRE(device.draws.size() == 1);
    CHECK((test::minCorner(device.draws[0]).x + test::maxCorner(device.draws[0]).x) / 2 ==
          Catch::Approx(320));
    for (const s32 ticks : {-1, LegendsDreamcastEntrance::kDurationTicks, 1000}) {
        CAPTURE(ticks);
        draw(512, ticks);
        CHECK(device.draws.empty());
    }
}
