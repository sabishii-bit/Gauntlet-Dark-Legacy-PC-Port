#include <catch2/catch_test_macros.hpp>

#include "engine/assets/TextureSet.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/app/GameCursor.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("marker cursor rejects absent or malformed pictures", "[cursor]") {
    CHECK_FALSE(GameCursor::fromMarker({}));
    CHECK_FALSE(GameCursor::fromMarker(Image{8, 8, {1, 2, 3}}));
    CHECK_FALSE(GameCursor::fromMarker(Image::filled(8, 8, Color::rgba(90, 20, 240, 0))));
    CHECK_FALSE(GameCursor::load(test::scratchDirectory("cursor-no-assets")));
}

TEST_CASE("marker reduction trims padding, points up-left and does not bleed hidden colours",
          "[cursor]") {
    Image marker = Image::filled(32, 24, Color::rgba(255, 0, 0, 0));
    for (u32 x = 4; x <= 20; ++x) {
        const u32 spread = (20 - x) / 3;
        for (u32 y = 12 - spread; y <= 12 + spread; ++y) {
            marker.setPixel(x, y, Color::rgba(0, 200, 80, 255));
        }
    }
    const auto cursor = GameCursor::fromMarker(marker);
    REQUIRE(cursor);
    CHECK(cursor->image.width == GameCursor::kSize);
    CHECK(cursor->image.height == GameCursor::kSize);
    CHECK(cursor->hotX < GameCursor::kSize / 2);
    CHECK(cursor->hotY < GameCursor::kSize / 2);
    CHECK(cursor->image.pixel(cursor->hotX, cursor->hotY).a > 0);
    bool antialiased = false;
    usize visible = 0;
    for (u32 y = 0; y < GameCursor::kSize; ++y) {
        for (u32 x = 0; x < GameCursor::kSize; ++x) {
            const Color color = cursor->image.pixel(x, y);
            if (color.a != 0) {
                ++visible;
                CHECK(color.r == 0);
                CHECK(color.g == 200);
                CHECK(color.b == 80);
                antialiased = antialiased || color.a < 255;
            }
        }
    }
    CHECK(visible > 50);
    CHECK(antialiased);
    CHECK(cursor->image.pixel(0, 0).a == 0);
    CHECK(cursor->image.pixel(GameCursor::kSize - 1, GameCursor::kSize - 1).a == 0);
    CHECK(marker.pixel(0, 0) == Color::rgba(255, 0, 0, 0));

    // Enlarging only the transparent border must not alter pointer size or hotspot.
    Image padded = Image::filled(64, 64, Color::rgba(0, 0, 0, 0));
    for (u32 y = 0; y < marker.height; ++y) {
        for (u32 x = 0; x < marker.width; ++x) {
            padded.setPixel(x + 10, y + 14, marker.pixel(x, y));
        }
    }
    const auto other = GameCursor::fromMarker(padded);
    REQUIRE(other);
    CHECK(other->hotX == cursor->hotX);
    CHECK(other->hotY == cursor->hotY);
    CHECK(other->image.pixels == cursor->image.pixels);
}

TEST_CASE("the desktop pointer decodes MARKER_LEFT directly from the native STATIC archive",
          "[cursor][assets]") {
    const auto root = test::assetOrSkip("STATIC/objects.ngc").parent_path().parent_path();
    TextureSet textures;
    REQUIRE(textures.load(root / "STATIC"));
    const auto marker = textures.find("MARKER_LEFT");
    REQUIRE(marker);
    const auto source = textures.image(*marker);
    const auto cursor = GameCursor::load(root);
    REQUIRE(cursor);
    const auto expected = GameCursor::fromMarker(source);
    REQUIRE(expected);
    CHECK(cursor->image.pixels == expected->image.pixels);
    CHECK(cursor->image.width == 40);
    CHECK(cursor->image.height == 40);
    CHECK(cursor->hotX < 20);
    CHECK(cursor->hotY < 20);
    CHECK(cursor->image.pixel(cursor->hotX, cursor->hotY).a > 0);
    usize visible = 0;
    usize purple = 0;
    for (u32 y = 0; y < cursor->image.height; ++y) {
        for (u32 x = 0; x < cursor->image.width; ++x) {
            const Color color = cursor->image.pixel(x, y);
            visible += color.a > 0 ? 1U : 0U;
            purple += color.a > 32 && color.r > color.g && color.b > color.g ? 1U : 0U;
        }
    }
    CHECK(visible > 80);
    CHECK(visible < 700);
    CHECK(purple > 3);
    CHECK(cursor->image.pixel(0, 0).a == 0);
    CHECK(cursor->image.pixel(GameCursor::kSize - 1, GameCursor::kSize - 1).a == 0);
    // Keep the generated pointer available for visual inspection without shipping
    // another converted asset or putting PNG decoding on the runtime path.
    writeFile(test::scratchDirectory("cursor-native") / "cursor.rgba", cursor->image.pixels);
}
} // namespace
