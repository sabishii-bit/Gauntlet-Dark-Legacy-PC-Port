#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Font.h"
#include "expansions/n64/formats/LegendsN64Text.h"
#include "expansions/n64/level/LegendsSumner.h"
#include "expansions/n64/menu/LegendsDialogue.h"
#include "expansions/n64/scenes/LegendsSumnerText.h"
#include "game/hud/LevelMessages.h"
#include "game/tower/HintMenu.h"

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

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8U);
    bytes.at(at + 1) = static_cast<u8>(value);
}

void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16U));
    put16(bytes, at + 2, static_cast<u16>(value));
}

/** A hud/objects.wad holding only the dialogue's 64x32 TEXTSCROLL, white I4 texels. */
std::vector<u8> scrollArchive() {
    constexpr usize kHeader = 8;
    constexpr usize kName = 32;
    constexpr usize kLoader = 48;
    constexpr usize kPixels = 56;
    constexpr usize kPixelBytes = usize{64} * 32 / 2;
    constexpr usize kDirectory = kPixels + kPixelBytes;
    constexpr usize kEntry = 12;
    std::vector<u8> bytes(kDirectory + 4 * kEntry, 0);
    put32(bytes, 0, static_cast<u32>(kDirectory));
    put32(bytes, 4, 4);
    put32(bytes, kHeader + 4, static_cast<u32>(kLoader));
    put16(bytes, kHeader + 12, 64);
    put16(bytes, kHeader + 14, 32);
    put16(bytes, kHeader + 16, static_cast<u16>(kPixelBytes / 8)); // eight-byte units
    bytes[kHeader + 18] = 4;                                       // intensity, four bits a texel
    constexpr std::string_view kTextureName = "TEXTSCROLL";
    std::ranges::copy(kTextureName, bytes.begin() + static_cast<std::ptrdiff_t>(kName));
    put32(bytes, kLoader, 0xDF000000); // the load display list ends at once
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(kPixels),
              bytes.begin() + static_cast<std::ptrdiff_t>(kDirectory), u8{0xff});
    const std::array<std::string_view, 4> tags{"TXHD", "NAME", "TXLD", "TEXR"};
    const std::array<usize, 4> offsets{kHeader, kName, kLoader, kPixels};
    const std::array<usize, 4> counts{1, kLoader - kName, kPixels - kLoader, kPixelBytes};
    for (usize i = 0; i < tags.size(); ++i) {
        const usize at = kDirectory + i * kEntry;
        std::ranges::copy(tags[i], bytes.begin() + static_cast<std::ptrdiff_t>(at));
        put32(bytes, at + 4, static_cast<u32>(offsets[i]));
        put32(bytes, at + 8, static_cast<u32>(counts[i]));
    }
    return bytes;
}

/** A native root with the synthetic font and scroll, and the topic title font when asked. */
std::filesystem::path syntheticRoot(std::string_view name, bool titleFont) {
    const auto root = test::scratchDirectory(name);
    std::filesystem::create_directories(root / "Font");
    std::filesystem::create_directories(root / "hud");
    writeFile(root / "Font/glfm3.fnt", smallFont());
    if (titleFont) {
        writeFile(root / "Font/bigfont.fnt", smallFont());
    }
    writeFile(root / "hud/objects.wad", scrollArchive());
    return root;
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

TEST_CASE("N64 Tower ceremony captions draw the text they are given and fade",
          "[legends][dialogue][tower-ceremony]") {
    test::FakeRenderDevice device;
    LegendsDialogue style;
    REQUIRE(style.load(device, syntheticRoot("n64-ceremony-captions", true)));
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    style.drawCeremonyCaption(canvas, "AA", 1, 512);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    const auto caption = device.draws.front();
    REQUIRE_FALSE(caption.vertices.empty());
    CHECK(caption.vertices.front().color == Color::white());
    device.draws.clear();
    // Tower e0860620's font5 at scale one and y30, centred on the view.
    constexpr f32 kScale = 512.0f / 416;
    canvas.begin(device, Mat4{1});
    style.text().draw(canvas, -256, static_cast<s32>(30 * kScale), "AA",
                      {.scale = kScale, .color = Color::white()});
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().vertices == caption.vertices);
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    style.drawCeremonyCaption(canvas, "AA", .5f, 512);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().vertices.front().color.a == 127);
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    style.drawCeremonyCaption(canvas, "AA", 0, 512);
    style.drawCeremonyCaption(canvas, "", 1, 512);
    canvas.end();
    CHECK(device.draws.empty());
}

TEST_CASE("N64 Tower ceremony captions draw the string table's line as the native bank's",
          "[legends][assets][native-dialogue][tower-ceremony]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "Font/glfm3.fnt") ||
        !std::filesystem::is_regular_file(root / "strlib.wad")) {
        SKIP("Native N64 fonts and strings are not installed");
    }
    test::FakeRenderDevice device;
    LegendsDialogue style;
    REQUIRE(style.load(device, root));
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto key = legendsN64CeremonyCaptionKey(541);
    REQUIRE(strings.has(key));
    const auto reference = formats::legendsN64StringReference(readFile(root / "strlib.wad"), 541);
    REQUIRE(reference);
    const auto bank =
        formats::decodeLegendsN64Text(readFile(root / "english" / (reference->bank + ".wad")));
    REQUIRE(reference->index < bank.size());
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    style.drawCeremonyCaption(canvas, strings.get(key), 1, 512);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    const auto caption = device.draws.front();
    CHECK(caption.texture->width() == 128);
    CHECK(caption.texture->height() == 64);
    REQUIRE_FALSE(caption.vertices.empty());
    CHECK(caption.vertices.front().color == Color::white());
    device.draws.clear();
    // The disc's own line, drawn as e086098c draws it, is what the table's must match.
    constexpr f32 kScale = 512.0f / 416;
    canvas.begin(device, Mat4{1});
    style.text().draw(canvas, -256, static_cast<s32>(30 * kScale), bank[reference->index],
                      {.scale = kScale, .color = Color::white()});
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().vertices == caption.vertices);
}

TEST_CASE("N64 popups keep their font and scroll without a usable topic title font",
          "[legends][dialogue]") {
    const auto root = syntheticRoot("n64-dialogue-title-font", false);
    const std::array topics{MenuItem{.text = "A", .code = 1}};
    const auto present = [&](bool titled) {
        test::FakeRenderDevice device;
        LegendsDialogue style;
        REQUIRE(style.load(device, root));
        REQUIRE(style.text().ready());
        REQUIRE(style.menuTextures());
        CHECK(style.menuTextures()->backdrop != nullptr);
        // Without font8 the topics fall back to the shared menu...
        CHECK(style.nativeTopics("A", topics).has_value() == titled);
        // ...while popups keep the native font and three-piece scroll.
        LevelMessages messages;
        messages.setPresentation(&style);
        const std::array<std::string, 1> prose{"AAA"};
        REQUIRE(messages.openImported(device, prose, nullptr));
        Canvas canvas;
        canvas.begin(device, Mat4{1});
        messages.draw(canvas);
        canvas.end();
        REQUIRE(device.draws.size() == 2);
        CHECK(device.draws.front().texture->width() == 64);
        CHECK(device.draws.front().vertices.size() == 18);
        REQUIRE_FALSE(device.draws.back().vertices.empty());
        CHECK(device.draws.back().vertices.front().color == Color::black());
    };
    SECTION("missing") {
        present(false);
    }
    SECTION("malformed") {
        writeFile(root / "Font/bigfont.fnt", std::vector<u8>(3));
        present(false);
    }
    SECTION("present") {
        writeFile(root / "Font/bigfont.fnt", smallFont());
        present(true);
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

TEST_CASE("N64 Sumner presents all six native choices with its two native fonts",
          "[legends][assets][dialogue][native-dialogue][sumner]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "Font/bigfont.fnt") ||
        !std::filesystem::is_regular_file(root / "_program/Tower.bin")) {
        SKIP("Native N64 Sumner assets are not installed");
    }
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto native = LegendsSumner::decode(readFile(root / "_program/game.bin"),
                                              readFile(root / "_program/Tower.bin"));
    const auto titleFont = formats::LegendsN64Font::parse(readFile(root / "Font/bigfont.fnt"));
    HintMenuLabels labels;
    labels.title = strings.get(legendsN64SumnerTextKey(native.titleId()));
    for (const auto& choice : native.choices()) {
        labels.topics.push_back({std::string{strings.get(legendsN64SumnerTextKey(choice.textId))},
                                 static_cast<s32>(choice.action)});
    }
    CHECK(labels.title == "Sumner");
    const std::array expected{"Continue",   "Shop",       "Rune Hint",
                              "World Hint", "Enemy Hint", "History"};
    REQUIRE(labels.topics.size() == expected.size());
    for (usize i = 0; i < expected.size(); ++i) {
        CHECK(labels.topics[i].text == expected[i]);
    }
    test::FakeRenderDevice device;
    LegendsDialogue style;
    REQUIRE(style.load(device, root));
    const auto definition = style.nativeTopics(labels.title, labels.topics);
    REQUIRE(definition);
    CHECK(definition->title.empty()); // The native font8 title is drawn separately.
    CHECK_FALSE(definition->showCursor);
    CHECK_FALSE(definition->parchmentFont);
    CHECK_FALSE(definition->garamondIntro);
    REQUIRE(style.menuTextures());
    HintMenuArt art;
    art.textures = *style.menuTextures();
    art.presentation = &style;
    HintMenu menu;
    menu.setArt(art);
    REQUIRE(menu.open(style.text(), {}, labels, definition));
    const Rect area = menu.topics().backdropArea();
    CHECK(area.width < 350);
    CHECK(area.height < 300);
    CHECK(area.y >= 0);
    CHECK(area.bottom() <= 320);
    Canvas canvas;
    const auto created = device.texturesCreated;
    canvas.begin(device, Mat4{1});
    menu.draw(canvas, style.text());
    canvas.end();
    CHECK(device.texturesCreated == created);
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws[0].texture == art.textures.backdrop);
    CHECK(device.draws[0].vertices.size() == 6); // Whole sheet for topics, not long-page slices.
    CHECK(device.draws[1].texture->width() == titleFont.atlas.width);
    CHECK(device.draws[1].texture->height() == titleFont.atlas.height);
    CHECK(device.draws[1].vertices.front().color == Color::rgba(0, 0, 132));
    CHECK(device.draws[2].texture != device.draws[1].texture);
    CHECK(device.draws[2].texture->width() == 128);
    CHECK(device.draws[2].texture->height() == 64);
    CHECK(device.draws[2].vertices.front().color == Color::rgba(0, 132, 0));
    // Mouse input reaches both trailing topics without widening the four-topic
    // base campaign's separately encoded online commands.
    for (usize i = 0; i < expected.size(); ++i) {
        const auto row = menu.topics().itemArea(i);
        MenuInput click;
        click.pointer = Vec2{row.x + row.width / 2, row.y + row.height / 2};
        click.pointerPressed = true;
        const auto chosen = menu.update(device, click, 1);
        CHECK(chosen.kind == HintMenuEvent::Kind::Asked);
        CHECK(chosen.topic == labels.topics[i].code);
    }
    CHECK_FALSE(native.adviceTextId(0x11, std::array<LegendsProgress, 1>{}));
    CHECK_FALSE(style.load(device, test::dataDirectory() / "missing-n64-ui"));
    CHECK_FALSE(style.text().ready());
    CHECK(style.menuTextures()->backdrop == nullptr);
    CHECK_FALSE(style.nativeTopics(labels.title, labels.topics));
}
