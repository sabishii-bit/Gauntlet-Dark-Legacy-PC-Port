#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudLayout.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Layout = LegendsDreamcastHudLayout;

/** A word the Dreamcast's boxes write: its id in the string table, where 1ST_READ.BIN keeps it,
 * and how the executable spells it. */
struct NativeWord {
    std::string_view key;
    u32 address = 0;
    std::string_view text;
};

/** The words of FUN_8c0c16e0's boxes and FUN_8c0bda40's prompt. IN TOWER is Dark Legacy's own
 * words too, which its boxes write. */
constexpr std::array<NativeWord, 8> kWords{{
    {.key = Layout::kWaitHereText, .address = 0x8c1256c0, .text = "WAIT HERE..."},
    {.key = Layout::kWaitText, .address = 0x8c1254cc, .text = "WAIT"},
    {.key = Layout::kInTowerQuestionText, .address = 0x8c1254d4, .text = "IN TOWER?"},
    {.key = Layout::kReturnText, .address = 0x8c1254e0, .text = "RETURN"},
    {.key = Layout::kToTowerQuestionText, .address = 0x8c1254e8, .text = "TO TOWER?"},
    {.key = Layout::kYesText, .address = 0x8c1254f4, .text = "YES"},
    {.key = Layout::kNoText, .address = 0x8c1254f8, .text = "NO"},
    {.key = Layout::kInTowerText, .address = 0x8c1256b4, .text = "IN TOWER"},
}};

TEST_CASE("the Dreamcast HUD's words are keyed", "[legends][dreamcast-hud][dreamcast-hud-text]") {
    StringTable text;
    REQUIRE(text.load(test::dataDirectory() / "text", "en"));
    for (const NativeWord& word : kWords) {
        CAPTURE(word.key);
        REQUIRE(text.has(word.key));
        CHECK(text.get(word.key) == word.text);
    }
    // The Dreamcast's own words live under its own keys; IN TOWER is Dark Legacy's key.
    for (const NativeWord& word : kWords) {
        CAPTURE(word.key);
        CHECK((word.key == Layout::kInTowerText || word.key.starts_with("legends.dreamcast.hud.")));
    }
    CHECK(Layout::kInTowerText == "hud.inTower");
}

TEST_CASE("every Dreamcast HUD word matches 1ST_READ.BIN exactly",
          "[legends][dreamcast-hud-text][assets]") {
    const auto path =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/disc/1ST_READ.BIN";
    if (!std::filesystem::exists(path)) {
        SKIP("optional Dreamcast executable is not installed");
    }
    const std::vector<u8> image = readFile(path);
    constexpr u32 kBase = 0x8c010000;
    constexpr usize kLongestWord = 64;
    /** The executable's text at `address`, up to its ending nought. */
    const auto nativeText = [&](u32 address) {
        REQUIRE(address >= kBase);
        usize offset = address - kBase;
        std::string found;
        while (offset < image.size() && image[offset] != 0 && found.size() < kLongestWord) {
            found.push_back(static_cast<char>(image[offset++]));
        }
        REQUIRE(offset < image.size());
        REQUIRE(image[offset] == 0);
        return found;
    };
    StringTable text;
    REQUIRE(text.load(test::dataDirectory() / "text", "en"));
    for (const NativeWord& word : kWords) {
        CAPTURE(word.key, word.address);
        REQUIRE(text.has(word.key));
        CHECK(text.get(word.key) == nativeText(word.address));
        CHECK(word.text == nativeText(word.address));
    }
}

} // namespace
