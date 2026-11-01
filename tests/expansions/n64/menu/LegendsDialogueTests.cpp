#include <algorithm>
#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Font.h"
#include "expansions/n64/menu/LegendsDialogue.h"
#include "game/hud/LevelMessages.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::vector<u8> smallFont() {
    std::vector<u8> bytes(56, 0);
    bytes[1] = 4;
    bytes[9] = 4;
    bytes[11] = 4;
    bytes[13] = 4;
    bytes[15] = 2;
    bytes[19] = 1;
    bytes[23] = 1;
    bytes[36] = 'A';
    bytes[39] = 3;
    bytes[40] = 4;
    bytes[48] = 0x0f;
    bytes[49] = 0x78;
    return bytes;
}
} // namespace

TEST_CASE("N64 modal fonts decode packed glyphs and coverage without GC font assumptions",
          "[legends][font][dialogue]") {
    const auto bytes = smallFont();
    const auto decoded = formats::LegendsN64Font::parse(bytes);
    REQUIRE(decoded.font.height() == 4);
    REQUIRE(decoded.font.spaceWidth() == 2);
    REQUIRE(decoded.font.glyph('A'));
    REQUIRE(decoded.font.glyph('a'));
    CHECK(decoded.font.glyph('a')->x == decoded.font.glyph('A')->x);
    CHECK(decoded.font.glyph('A')->rows == 4);
    CHECK(decoded.atlas.pixels[3] == 0);
    CHECK(decoded.atlas.pixels[7] == 255);
    CHECK(decoded.atlas.pixels[11] == 119);
    CHECK(decoded.atlas.pixels[15] == 136);
    CHECK(decoded.atlas.pixels[0] == 255);
    for (usize size = 0; size < bytes.size(); ++size) {
        CHECK_THROWS_AS(formats::LegendsN64Font::parse(std::span{bytes}.first(size)), FormatError);
    }
    for (const usize offset : {usize{1}, usize{2}, usize{9}, usize{11}, usize{13}, usize{15},
                               usize{23}, usize{36}, usize{39}, usize{40}}) {
        auto bad = bytes;
        bad[offset] = offset == 1 || offset == 2 || offset == 39 || offset == 40 ? 255 : 0;
        CHECK_THROWS_AS(formats::LegendsN64Font::parse(bad), FormatError);
    }
}

TEST_CASE("N64 native popup uses its font and three-piece modal through shared paging",
          "[legends][assets][dialogue][native-dialogue]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "Font/glfm3.fnt")) {
        SKIP("Native N64 fonts are not installed");
    }
    const auto decoded = formats::LegendsN64Font::parse(readFile(root / "Font/glfm3.fnt"));
    CHECK(decoded.font.height() == 11);
    CHECK(decoded.font.spaceWidth() == 7);
    CHECK(decoded.atlas.width == 128);
    CHECK(decoded.atlas.height == 64);
    REQUIRE(decoded.font.glyph('A'));
    REQUIRE(decoded.font.glyph('a'));
    test::FakeRenderDevice device;
    LegendsDialogue style;
    REQUIRE(style.load(device, root));
    LevelMessages messages;
    messages.setPresentation(&style);
    const std::array<std::string, 1> prose{"THE TOWER IS YOUR SAFE HAVEN."};
    REQUIRE(messages.openImported(device, prose, nullptr));
    CHECK(messages.scroll().pageCount() == 1);
    CHECK_FALSE(messages.scroll().burning());
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    messages.draw(canvas);
    canvas.end();
    REQUIRE(device.draws.size() == 2);
    const auto& backdrop = device.draws.front();
    CHECK(backdrop.texture->width() == 64);
    CHECK(backdrop.texture->height() == 32);
    REQUIRE(backdrop.vertices.size() == 18);
    CHECK(backdrop.vertices[0].uv.y == Catch::Approx(.5f / 32));
    const auto& lettering = device.draws.back();
    CHECK(lettering.texture->width() == 128);
    CHECK(lettering.texture->height() == 64);
    CHECK(lettering.vertices.front().color == Color::black());
    const auto hold = messages.step(ScrollBox::kHoldTicks, 1);
    CHECK_FALSE(hold.stopVoice);
    CHECK(messages.active());
    const auto dismiss = messages.step(1, 1);
    CHECK(dismiss.stopVoice);
    CHECK_FALSE(dismiss.burnSound);
    CHECK_FALSE(messages.active());
    // Oversized words and multi-page documents stay in the selected native layout.
    const std::array<std::string, 1> longText{std::string(2000, 'W')};
    REQUIRE(messages.openImported(device, longText, nullptr));
    CHECK(messages.scroll().pageCount() > 1);
    for (const auto& line : messages.scroll().lines()) {
        CHECK(style.text().measure(line, style.scale()) <= style.textWidth());
    }
    CHECK(messages.scroll().area().bottom() < 320);
    messages.clear();
    CHECK_FALSE(messages.active());
    CHECK_FALSE(messages.openImported(device, prose, nullptr));
}
