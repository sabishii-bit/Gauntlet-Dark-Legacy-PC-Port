#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/widgets/MenuFont.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** The values of the `powerup.*` entries of the English text, as the file holds them. */
std::vector<std::string> powerupNames() {
    const std::string text = readTextFile(test::dataDirectory() / "text" / "en.json");
    std::vector<std::string> names;
    usize at = 0;
    while ((at = text.find("\"powerup.", at)) != std::string::npos) {
        const usize idEnd = text.find('"', at + 1);
        const usize open = text.find('"', idEnd + 1);
        const usize close = text.find('"', open + 1);
        if (idEnd == std::string::npos || open == std::string::npos || close == std::string::npos) {
            break;
        }
        names.push_back(text.substr(open + 1, close - open - 1));
        at = close + 1;
    }
    return names;
}

TEST_CASE("the menu font gains a hyphen cut from its E, and every powerup name draws whole",
          "[game][menu][font][unpacked]") {
    const auto root = test::unpackedOrSkip("fonts/font32.json").parent_path().parent_path();
    BitmapFont plain;
    REQUIRE(plain.load(root / "fonts/font32.json", 16));
    CHECK(plain.glyph('-') == nullptr); // the artists never drew one
    BitmapFont font;
    REQUIRE(loadMenuFont(font, root, 16));
    const BitmapGlyph* hyphen = font.glyph('-');
    const BitmapGlyph* e = font.glyph('E');
    REQUIRE(hyphen != nullptr);
    REQUIRE(e != nullptr);
    CHECK(hyphen->x == e->x + kMenuFontHyphen.left);
    CHECK(hyphen->y == e->y + kMenuFontHyphen.top);
    CHECK(hyphen->columns == kMenuFontHyphen.columns);
    CHECK(hyphen->rows == kMenuFontHyphen.rows);
    CHECK(hyphen->width == kMenuFontHyphen.width);
    CHECK(hyphen->lead == kMenuFontHyphen.lead);
    CHECK(hyphen->top == kMenuFontHyphen.at);
    CHECK(font.glyphCount() == plain.glyphCount() + 1);
    // Every character of every powerup name has a cell, or is a space.
    const auto names = powerupNames();
    REQUIRE(names.size() > 60);
    bool hyphenated = false;
    for (const std::string& name : names) {
        CAPTURE(name);
        for (const char c : name) {
            hyphenated = hyphenated || c == '-';
            CHECK((c == ' ' || font.glyph(static_cast<u8>(c)) != nullptr));
        }
    }
    CHECK(hyphenated); // X-Ray among them
}

TEST_CASE("the menu font loader reports a missing font", "[game][menu][font]") {
    BitmapFont font;
    CHECK_FALSE(loadMenuFont(font, test::scratchDirectory("menu-font-missing"), 16));
    CHECK_FALSE(font.loaded());
}

} // namespace
