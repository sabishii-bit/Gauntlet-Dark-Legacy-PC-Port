#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Text.h"
#include "expansions/n64/scenes/LegendsScrolls.h"
#include "expansions/n64/scenes/LegendsSumnerText.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("N64 Sumner localization keys retain native identities without optional assets",
          "[legends][sumner][text]") {
    CHECK(legendsN64SumnerTextKey(441) == "legends.n64.sumner.text.441");
    CHECK(legendsN64SumnerTextKey(492) == "legends.n64.sumner.text.492");
    CHECK(legendsN64SumnerTextKey(559) == "legends.n64.sumner.text.559");
    for (const u32 identity : {0U, 12U, 37U, 50U, 425U, 427U, 431U, 440U, 446U, 491U, 560U,
                               std::numeric_limits<u32>::max()}) {
        CAPTURE(identity);
        CHECK(legendsN64SumnerTextKey(identity).empty());
    }
    StringTable text;
    REQUIRE(text.load(test::dataDirectory() / "text", "en"));
    usize count = 0;
    for (u32 identity = 1; identity <= 559; ++identity) {
        const auto key = legendsN64SumnerTextKey(identity);
        if (key.empty()) {
            continue;
        }
        CAPTURE(identity, key);
        REQUIRE(text.has(key));
        CHECK(text.get(key).empty() == (identity == 521 || identity == 522 || identity == 526));
        ++count;
    }
    CHECK(count == 87);
    CHECK(text.get(legendsN64SumnerTextKey(441)) == "Sumner");
    CHECK(text.get(legendsN64SumnerTextKey(442)) == "Rune Hint");
    CHECK(text.get(legendsN64SumnerTextKey(443)) == "World Hint");
    CHECK(text.get(legendsN64SumnerTextKey(444)) == "Enemy Hint");
    CHECK(text.get(legendsN64SumnerTextKey(445)) == "History");
}

TEST_CASE("Every N64 Sumner English line matches its native string bank exactly",
          "[legends][sumner][text][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "strlib.wad")) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    const auto library = readFile(root / "strlib.wad");
    StringTable text;
    REQUIRE(text.load(test::dataDirectory() / "text", "en"));
    std::map<std::string, std::vector<std::string>> banks;
    usize count = 0;
    for (u32 identity = 1; identity <= 559; ++identity) {
        const auto key = legendsN64SumnerTextKey(identity);
        if (key.empty()) {
            continue;
        }
        CAPTURE(identity, key);
        const auto reference = formats::legendsN64StringReference(library, identity);
        REQUIRE(reference);
        auto bank = banks.find(reference->bank);
        if (bank == banks.end()) {
            bank = banks
                       .emplace(reference->bank,
                                formats::decodeLegendsN64Text(
                                    readFile(root / "english" / (reference->bank + ".wad"))))
                       .first;
        }
        REQUIRE(reference->index < bank->second.size());
        REQUIRE(text.has(key));
        CHECK(text.get(key) == bank->second[reference->index]);
        ++count;
    }
    CHECK(count == 87);
    CHECK(banks.size() == 3);
}

TEST_CASE("N64 Sumner History catalogs only authored external advice pages",
          "[legends][sumner][text]") {
    StringTable text;
    REQUIRE(text.load(test::dataDirectory() / "text", "en"));
    usize count = 0;
    for (u32 page = 0; page <= 255; ++page) {
        const auto key = legendsN64SumnerHistoryKey(static_cast<u8>(page));
        const bool authored = page == 1 || page == 2 || (page >= 10 && page <= 17);
        CAPTURE(page, key);
        REQUIRE(key.empty() == !authored);
        if (authored) {
            REQUIRE(text.has(key));
            CHECK_FALSE(text.get(key).empty());
            ++count;
        }
    }
    CHECK(count == 10);
}

TEST_CASE("Localized N64 Sumner History preserves the external native documents",
          "[legends][sumner][text][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "advice/adv_t1_01")) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    const auto program = readFile(root / "_program/game.bin");
    StringTable text;
    REQUIRE(text.load(test::dataDirectory() / "text", "en"));
    for (u8 page = 1; page <= 17; ++page) {
        CAPTURE(page);
        const auto file = root / "advice" / std::format("adv_t1_{:02}", page);
        const auto key = legendsN64SumnerHistoryKey(page);
        CHECK(std::filesystem::is_regular_file(file) == !key.empty());
        const auto decoded = loadLegendsN64Scroll(root / "levels/tower2", program, page);
        if (key.empty()) {
            CHECK(decoded.empty());
            continue;
        }
        REQUIRE(decoded.size() == 1);
        REQUIRE(text.has(key));
        CHECK(text.get(key) == decoded.front());
        // No line-ending or layout normalization is allowed in the catalog.
        const auto raw = readFile(file);
        std::string source;
        for (const u8 value : raw) {
            if (value == 0) {
                break;
            }
            source.push_back(static_cast<char>(value));
        }
        CHECK(text.get(key) == source);
    }
}
} // namespace
