#include <algorithm>
#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Text.h"
#include "game/hud/LevelMessages.h"
#include "expansions/shared/level/LegendsItems.h"
#include "expansions/n64/scenes/LegendsScrolls.h"

namespace {
using namespace gdl;
using namespace gdl::formats;
using namespace gdl::game;
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        bytes.at(at + i) = static_cast<u8>(value >> (24 - i * 8));
    }
}
std::vector<u8> textWad() {
    std::vector<u8> bytes(48);
    put32(bytes, 0, 24);
    put32(bytes, 4, 2);
    bytes[8] = 'H';
    bytes[9] = 'i';
    bytes[11] = '!';
    put32(bytes, 16, 8);
    put32(bytes, 20, 11);
    put32(bytes, 24, 0x53545253);
    put32(bytes, 28, 8);
    put32(bytes, 32, 5);
    put32(bytes, 36, 0x00544f43);
    put32(bytes, 40, 16);
    put32(bytes, 44, 2);
    return bytes;
}
std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::exists(root / "_program/game.bin")) {
        SKIP("N64 Legends not extracted");
    }
    return root;
}
} // namespace

TEST_CASE("N64 text uses file-relative big-endian TOC words", "[legends][scrolls][formats]") {
    CHECK(decodeLegendsN64Text(textWad()) == std::vector<std::string>{"Hi", "!"});
    auto bytes = textWad();
    SECTION("out of range directory") {
        put32(bytes, 0, 0xfffffff0);
    }
    SECTION("duplicate strings") {
        put32(bytes, 36, 0x53545253);
    }
    SECTION("offset points outside string chunk") {
        put32(bytes, 16, 16);
    }
    SECTION("unterminated text") {
        bytes[12] = 'x';
    }
    SECTION("oversized TOC") {
        put32(bytes, 44, 65537);
    }
    CHECK_THROWS_AS(decodeLegendsN64Text(bytes), FormatError);
}

TEST_CASE("Native scroll registry resolves every text bank without guessed realm names",
          "[legends][scrolls][assets]") {
    const auto root = nativeRoot();
    const auto program = readFile(root / "_program/game.bin");
    const auto library = readFile(root / "strlib.wad");
    const auto tower = legendsN64ScrollContext(program, "tower2");
    REQUIRE(tower);
    CHECK(tower->realm == 'T');
    CHECK(tower->level == 1);
    const auto mountain = legendsN64ScrollContext(program, "mount1");
    REQUIRE(mountain);
    CHECK(mountain->firstString == 50);
    const auto first = legendsN64StringReference(library, 50);
    REQUIRE(first);
    CHECK(first->bank == "scroll_mtn");
    CHECK(first->index == 0);
    CHECK_FALSE(legendsN64StringReference(library, 0));
    CHECK_FALSE(legendsN64StringReference(library, 65535));
    const auto advice = loadLegendsN64Scroll(root / "levels/tower2", program, 1);
    REQUIRE(advice.size() == 1);
    CHECK(advice.front().find("Garm") != std::string::npos);
    const auto text = decodeLegendsN64Text(readFile(root / "english/scroll_mtn.wad"));
    REQUIRE(text.size() == 11);
    CHECK(loadLegendsN64Scroll(root / "levels/mount1", program, 1) ==
          std::vector<std::string>{text.front()});
    for (const auto& entry : std::filesystem::directory_iterator(root / "english")) {
        CAPTURE(entry.path());
        CHECK_FALSE(decodeLegendsN64Text(readFile(entry.path())).empty());
    }
    CHECK(loadLegendsN64Scroll(root / "levels/unknown", program, 1).empty());
    CHECK(loadLegendsN64Scroll(root / "levels/tower2", program, 0).empty());
}

TEST_CASE("Imported scrolls require real text and reuse bounded GDL pages", "[legends][scrolls]") {
    LegendsPickup pickup;
    pickup.offer = {static_cast<s32>(ItemKind::Scroll), 1};
    CharacterSave save;
    CHECK_FALSE(takeLegendsPickup(pickup, LegendsPlatform::N64, save).took());
    pickup.scrollPages = {"Native scroll text"};
    pickup.active = false;
    CHECK_FALSE(takeLegendsPickup(pickup, LegendsPlatform::N64, save).took());
    pickup.active = true;
    CHECK(takeLegendsPickup(pickup, LegendsPlatform::N64, save).outcome ==
          ItemTaking::Outcome::Shown);
    CHECK_FALSE(takeLegendsPickup(pickup, LegendsPlatform::N64, save).took());

    const auto assets = test::assetOrSkip("STATIC").parent_path();
    test::FakeRenderDevice device;
    TextureSet textures;
    LevelMessages messages;
    messages.load(device, textures, assets, nullptr);
    std::string longText;
    for (s32 i = 0; i < 100; ++i) {
        longText += "The native message must fit on the scroll. ";
    }
    const std::array documents{longText};
    REQUIRE(messages.openImported(device, documents, nullptr));
    CHECK(messages.active());
    CHECK_FALSE(messages.look());
    REQUIRE(messages.scroll().pageCount() > 1);
    for (usize i = 0; i < messages.scroll().pageCount(); ++i) {
        CHECK(messages.scroll().lines().size() <= 9);
        for (const auto& line : messages.scroll().lines()) {
            CHECK(messages.text().measure(line, .5f) <= ScrollBox::kMaxWidth - ScrollBox::kMargin);
        }
        messages.step(16, 0);
        messages.step(1, 1);
    }
}
