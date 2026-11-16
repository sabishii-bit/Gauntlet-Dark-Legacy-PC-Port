#include <array>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Text.h"
#include "expansions/n64/hud/LegendsN64HudText.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** The class words of english/game.wad the N64's panels write (e005a288), Pojo's among them. */
constexpr std::array<s32, 10> kClassWords{129, 130, 131, 132, 133, 134, 135, 136, 137, 138};

TEST_CASE("the N64 HUD's class words are keyed by their native ids",
          "[legends][n64-hud][n64-hud-text]") {
    StringTable text;
    REQUIRE(text.load(test::dataDirectory() / "text", "en"));
    for (const s32 native : kClassWords) {
        CAPTURE(native);
        const std::string key = legendsN64HudTextKey(native);
        CHECK(key == "legends.n64.hud.text." + std::to_string(native));
        REQUIRE(text.has(key));
        CHECK_FALSE(text.get(key).empty());
    }
    CHECK(text.get(legendsN64HudTextKey(129)) == "Warrior");
    CHECK(text.get(legendsN64HudTextKey(133)) == "Pojo");
    CHECK(text.get(legendsN64HudTextKey(138)) == "Sumner");
}

TEST_CASE("every N64 HUD class word matches its native string bank exactly",
          "[legends][n64-hud-text][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "strlib.wad")) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    const auto library = readFile(root / "strlib.wad");
    StringTable text;
    REQUIRE(text.load(test::dataDirectory() / "text", "en"));
    for (const s32 native : kClassWords) {
        CAPTURE(native);
        const auto reference =
            formats::legendsN64StringReference(library, static_cast<u32>(native));
        REQUIRE(reference);
        CHECK(reference->bank == "game");
        const auto bank =
            formats::decodeLegendsN64Text(readFile(root / "english" / (reference->bank + ".wad")));
        REQUIRE(reference->index < bank.size());
        const std::string key = legendsN64HudTextKey(native);
        REQUIRE(text.has(key));
        CHECK(text.get(key) == bank[reference->index]);
    }
}

} // namespace
