#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Font.h"
#include "expansions/n64/formats/LegendsN64Text.h"
#include "expansions/n64/hud/LegendsN64Frame.h"
#include "expansions/n64/level/LegendsSumner.h"
#include "expansions/n64/menu/LegendsDialogue.h"
#include "expansions/n64/scenes/LegendsSumnerText.h"
#include "game/hud/LevelMessages.h"
#include "game/tower/HintMenu.h"
#include "game/tower/TowerGateways.h"
#include "game/widgets/MenuInput.h"
#include "game/widgets/OptionMenu.h"

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

/** The corners of what a draw's vertices cover, in the units they were drawn in. */
struct Reach {
    Vec2 least{std::numeric_limits<f32>::max()};
    Vec2 most{std::numeric_limits<f32>::lowest()};
};

Reach reachOf(const test::RecordedDraw& draw) {
    Reach reach;
    for (const ImmediateVertex& vertex : draw.vertices) {
        reach.least = glm::min(reach.least, Vec2{vertex.position});
        reach.most = glm::max(reach.most, Vec2{vertex.position});
    }
    return reach;
}

/** A word of `count` letters A, the synthetic font's only glyph, three columns each and four
 * apart with the unit the N64 writes before each. */
std::string letters(usize count) {
    // Braces would make the count one of the letters.
    std::string word(count, 'A');
    return word;
}
} // namespace

TEST_CASE("N64 modal fonts decode packed glyphs and coverage without GC font assumptions",
          "[legends][font][dialogue]") {
    const auto bytes = smallFont();
    const auto decoded = formats::LegendsN64Font::parse(bytes);
    REQUIRE(decoded.font.height() == 4);
    // The file's space of two, and the unit the N64 writes before every character.
    REQUIRE(decoded.font.spaceWidth() == 3);
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
    style.drawCeremonyCaption(canvas, "AA", 1);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    const auto caption = device.draws.front();
    REQUIRE_FALSE(caption.vertices.empty());
    CHECK(caption.vertices.front().color == Color::white());
    device.draws.clear();
    // Tower e0860620's font5 at scale one and y30 of the N64's frame, centred on its x208.
    canvas.begin(device, Mat4{1});
    {
        const CanvasFrame frame{canvas, LegendsN64Frame::transform()};
        style.text().draw(canvas, -208, 30, "AA", {.scale = 1, .color = Color::white()});
    }
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().vertices == caption.vertices);
    CHECK(caption.transform == LegendsN64Frame::transform());
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    style.drawCeremonyCaption(canvas, "AA", .5f);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().vertices.front().color.a == 127);
    device.draws.clear();
    canvas.begin(device, Mat4{1});
    style.drawCeremonyCaption(canvas, "AA", 0);
    style.drawCeremonyCaption(canvas, "", 1);
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
    style.drawCeremonyCaption(canvas, strings.get(key), 1);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    const auto caption = device.draws.front();
    CHECK(caption.texture->width() == 128);
    CHECK(caption.texture->height() == 64);
    REQUIRE_FALSE(caption.vertices.empty());
    CHECK(caption.vertices.front().color == Color::white());
    device.draws.clear();
    // The disc's own line, drawn as e086098c draws it in the N64's frame, is what the table's
    // must match.
    canvas.begin(device, Mat4{1});
    {
        const CanvasFrame frame{canvas, LegendsN64Frame::transform()};
        style.text().draw(canvas, -208, 30, bank[reference->index],
                          {.scale = 1, .color = Color::white()});
    }
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws.front().vertices == caption.vertices);
    CHECK(device.draws.front().transform == caption.transform);
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

TEST_CASE("N64 dialogue draws in the N64's frame, as tall as the video hardware made it",
          "[legends][n64-dialogue]") {
    test::FakeRenderDevice device;
    LegendsDialogue style;
    REQUIRE(style.load(device, syntheticRoot("n64-dialogue-frame", false)));
    // A page is laid out in the frame's units, as e00717a0/e0071a10 lay it out: the 350-wide
    // long-page scroll at (40, 40), sixteen rows taller than its two lines of four and the
    // port's prompt line, the text six rows under its top.
    REQUIRE(style.scale() == 1);
    const DialoguePage page = style.layout("AAA\nA", style.scale(), "A");
    CHECK(page.area == Rect{40, 40, 350, 2 * 4 + 16 + 4});
    CHECK(page.textTop == 46);
    CHECK(page.promptY == 46 + 2 * 4 + 3);
    // On a canvas placed as a scene places it, every draw goes through the frame.
    const Mat4 screen = makeVirtualScreenTransform(Mat4{1}, 512, 384, 640, 448);
    Canvas canvas;
    canvas.begin(device, screen);
    style.draw(canvas, page, style.scale(), "A", 255);
    canvas.end();
    REQUIRE(device.draws.size() == 2); // the scroll, then its lines and prompt
    for (const auto& draw : device.draws) {
        CHECK(draw.transform == screen * LegendsN64Frame::transform());
    }
    const auto& scroll = device.draws.front();
    CHECK(scroll.texture == style.menuTextures()->backdrop);
    REQUIRE(scroll.vertices.size() == 18); // the caps and the stretched rows between them
    const Reach covered = reachOf(scroll);
    CHECK(covered.least == Vec2{40, 40});
    CHECK(covered.most == Vec2{390, 40 + page.area.height});
    // On the canvas a frame row is 1.7160 tall, as the video interface scales it, where the
    // uniform 512/416 made it 1.2308.
    const Mat4 frame = LegendsN64Frame::transform();
    const f32 canvasTop = (frame * Vec4{covered.least, 0, 1}).y;
    const f32 canvasBottom = (frame * Vec4{covered.most, 0, 1}).y;
    CHECK(canvasBottom - canvasTop == Catch::Approx(page.area.height * 1.7160f).epsilon(1e-4));
    // The lines start 60 in from the scroll's left, their first glyph the N64's unit further,
    // and the prompt follows them.
    const Reach text = reachOf(device.draws.back());
    CHECK(text.least.x == Catch::Approx(40 + 60 + 1 + TextPainter::kCellInset));
    CHECK(text.least.y == Catch::Approx(46 + TextPainter::kCellInset));
    CHECK(text.most.y ==
          Catch::Approx(static_cast<f32>(page.promptY + 4) - TextPainter::kCellInset));
}

TEST_CASE("N64 dialogue breaks a line as the N64 does", "[legends][n64-dialogue]") {
    test::FakeRenderDevice device;
    LegendsDialogue style;
    REQUIRE(style.load(device, syntheticRoot("n64-dialogue-breaks", false)));
    // game.bin FUN_e00189c0 breaks a line at its last space once it reaches the long page's
    // wrap of 240; the shared pager breaks once a line passes textWidth.
    REQUIRE(style.scale() == 1);
    CHECK(style.textWidth() == 239);
    /** The lines the shared pager lays `prose` out in, on the N64's scroll. */
    const auto linesOf = [&](const std::string& prose) {
        LevelMessages messages;
        messages.setPresentation(&style);
        REQUIRE(messages.openImported(device, std::array{prose}, nullptr));
        return messages.scroll().lines();
    };
    // The synthetic font's A is three columns and its space two, and FUN_e00189c0 counts a unit
    // before each character: a letter takes four and a space three, so 29 and 30 letters make
    // 239.
    REQUIRE(style.text().measure("A A", style.scale()) == 4 + 3 + 4);
    const std::string fits = letters(29) + ' ' + letters(30);
    REQUIRE(style.text().measure(fits, style.scale()) == 239);
    CHECK(linesOf(fits) == std::vector{fits});
    // Words of 12, 12, 11, 11 and 11 letters make 240: the line breaks at its last space.
    const std::string first =
        letters(12) + ' ' + letters(12) + ' ' + letters(11) + ' ' + letters(11);
    const std::string reaches = first + ' ' + letters(11);
    REQUIRE(style.text().measure(reaches, style.scale()) == 240);
    CHECK(linesOf(reaches) == std::vector{first, letters(11)});
}

TEST_CASE("the N64 topic scroll, title and rows take the N64's frame, and only the glyphs keep "
          "one scale",
          "[legends][n64-dialogue]") {
    test::FakeRenderDevice device;
    LegendsDialogue style;
    REQUIRE(style.load(device, syntheticRoot("n64-dialogue-topics", true)));
    const std::array topics{MenuItem{.text = "A", .code = 1}, MenuItem{.text = "AA", .code = 2},
                            MenuItem{.text = "AAA", .code = 3}};
    const auto definition = style.nativeTopics("A", topics);
    REQUIRE(definition);
    // e0046000 / e0041894 in the frame, both synthetic fonts four rows tall: the title takes
    // 4 x 1.5 and a 15-row gap, and the rows four and a five-row gap each, 43 rows with the last
    // gap dropped, centred on row 82.5, the middle of the top three quarters, so from row 61.5.
    // The widest line is AAA's twelve, three glyphs of three columns and the N64's unit before
    // each: the scroll stands 30 columns beyond its middle six on either side and ten rows above
    // and below, 72 by 63 from (172, 51.5).
    using Frame = LegendsN64Frame;
    const Vec2 corner = Frame::toCanvas({172, 51.5f});
    CHECK(definition->backdropX == static_cast<s32>(corner.x));
    CHECK(definition->backdropY == static_cast<s32>(corner.y));
    CHECK(definition->backdropWidth == static_cast<s32>(72 * Frame::kColumn));
    CHECK(definition->backdropHeight == static_cast<s32>(63 * Frame::kRow));
    // The rows stand under the title from row 82.5, nine rows apart, centred on the frame's
    // middle column.
    REQUIRE(definition->itemPositions.size() == topics.size());
    for (usize i = 0; i < topics.size(); ++i) {
        CAPTURE(i);
        const f32 row = 82.5f + 9.0f * static_cast<f32>(i);
        CHECK(definition->itemPositions[i].x == -256);
        CHECK(definition->itemPositions[i].y == Catch::Approx(Frame::toCanvas({0, row}).y));
    }
    // OptionMenu draws the rows' glyphs at one scale: the uniform one the whole menu once took.
    CHECK(definition->scale == LegendsDialogue::kTopicGlyphScale);
    CHECK(LegendsDialogue::kTopicGlyphScale == 512.0f / 416.0f);
    // A pointer picks each row where the frame put it.
    OptionMenu menu;
    menu.open(*definition, style.text(), MenuScreen{});
    for (usize i = 0; i < topics.size(); ++i) {
        CAPTURE(i);
        const Rect row = menu.itemArea(i);
        CHECK(row.y == static_cast<f32>(static_cast<s32>(definition->itemPositions[i].y)));
        MenuInput click;
        click.pointer = Vec2{row.x + row.width / 2, row.y + row.height / 2};
        click.pointerPressed = true;
        CHECK(menu.pointerChoice(click) == i);
    }
    // The title stands the scroll's ten-row margin under its top, its glyphs at the rows' scale.
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    REQUIRE(style.drawTopicsBackdrop(canvas, menu, "A"));
    canvas.end();
    REQUIRE(device.draws.size() == 2);
    const Rect scroll = menu.backdropArea();
    CHECK(device.draws.front().texture == style.menuTextures()->backdrop);
    CHECK(reachOf(device.draws.front()).least == Vec2{scroll.x, scroll.y});
    const auto title = formats::LegendsN64Font::parse(smallFont());
    const test::FakeTexture sheet{title.atlas.width, title.atlas.height};
    TextPainter heading;
    heading.setFont(&title.font, &sheet);
    test::FakeRenderDevice expected;
    canvas.begin(expected, Mat4{1});
    heading.draw(
        canvas, -256, static_cast<s32>(scroll.y + 10 * Frame::kRow), "A",
        {.scale = 1.5f * LegendsDialogue::kTopicGlyphScale, .color = Color::rgba(0, 0, 132)});
    canvas.end();
    REQUIRE(expected.draws.size() == 1);
    CHECK(device.draws.back().vertices == expected.draws.front().vertices);
}

TEST_CASE("N64 native popup uses its font and three-piece modal through shared paging",
          "[legends][assets][dialogue][native-dialogue]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "Font/glfm3.fnt")) {
        SKIP("Native N64 fonts are not installed");
    }
    const auto decoded = formats::LegendsN64Font::parse(readFile(root / "Font/glfm3.fnt"));
    CHECK(decoded.font.height() == 11);
    // glfm3's space of seven and the N64's unit before it.
    CHECK(decoded.font.spaceWidth() == 8);
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
    // e00717a0/e0071a10's long page, in the N64's frame: 350 wide at (40, 40), as tall as its
    // lines and sixteen rows more.
    const auto lines = static_cast<f32>(messages.scroll().lines().size());
    CHECK(messages.scroll().area() == Rect{40, 40, 350, lines * 11 + 16});
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
    CHECK(backdrop.transform == LegendsN64Frame::transform());
    const auto& lettering = device.draws.back();
    CHECK(lettering.texture->width() == 128);
    CHECK(lettering.texture->height() == 64);
    CHECK(lettering.vertices.front().color == Color::black());
    // The text starts six rows under the scroll's top and 60 in, its first glyph the N64's unit
    // further, half a texel inside its cells.
    const Reach text = reachOf(lettering);
    CHECK(text.least.x == Catch::Approx(40 + 60 + 1 + TextPainter::kCellInset));
    CHECK(text.least.y == Catch::Approx(40 + 6 + TextPainter::kCellInset));
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
    // A full page of twelve lines stays within the frame rows the video interface shows.
    CHECK(messages.scroll().lines().size() == style.linesPerPage());
    CHECK(messages.scroll().area().bottom() == 40 + 12 * 11 + 16);
    CHECK(messages.scroll().area().bottom() <= LegendsN64Frame::kRowsShown);
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

TEST_CASE("N64 Sumner's gateway row and his list of places stand whole on his own scroll",
          "[legends][assets][dialogue][native-dialogue][sumner][gateways]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "Font/bigfont.fnt") ||
        !std::filesystem::is_regular_file(root / "_program/Tower.bin")) {
        SKIP("Native N64 Sumner assets are not installed");
    }
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto native = LegendsSumner::decode(readFile(root / "_program/game.bin"),
                                              readFile(root / "_program/Tower.bin"));
    HintMenuLabels labels;
    labels.title = strings.get(legendsN64SumnerTextKey(native.titleId()));
    for (const auto& choice : native.choices()) {
        // No native action answers to the gateway row's code.
        CHECK(static_cast<s32>(choice.action) != kGatewayTopic);
        labels.topics.push_back({std::string{strings.get(legendsN64SumnerTextKey(choice.textId))},
                                 static_cast<s32>(choice.action)});
    }
    labels.topics.push_back({std::string{strings.get(kGatewayTopicText)}, kGatewayTopic});
    test::FakeRenderDevice device;
    LegendsDialogue style;
    REQUIRE(style.load(device, root));
    // Every letter of the new words is one the native font draws.
    const std::array words{strings.get(kGatewayTopicText), strings.get(kGatewayHomeText),
                           strings.get("legends.n64.gateway.label"),
                           strings.get("legends.dreamcast.gateway.label")};
    REQUIRE(style.text().font());
    for (const std::string_view word : words) {
        for (const char letter : word) {
            CAPTURE(word, letter);
            CHECK(
                (letter == ' ' || style.text().font()->glyph(static_cast<u8>(letter)) != nullptr));
        }
    }
    HintMenuArt art;
    art.textures = *style.menuTextures();
    art.presentation = &style;
    HintMenu menu;
    menu.setArt(art);
    /** Every row of `shown` whole on the scroll, and the scroll on the screen. */
    const auto checkWhole = [](const OptionMenu& shown) {
        const Rect scroll = shown.backdropArea();
        CHECK(scroll.x >= 0);
        CHECK(scroll.x + scroll.width <= 512);
        CHECK(scroll.y >= 0);
        CHECK(scroll.bottom() <= 384);
        for (usize i = 0; i < shown.definition().items.size(); ++i) {
            CAPTURE(i, shown.definition().items[i].text);
            const Rect row = shown.itemArea(i);
            CHECK(row.x >= scroll.x);
            CHECK(row.x + row.width <= scroll.x + scroll.width);
            CHECK(row.y >= scroll.y);
            CHECK(row.y + row.height <= scroll.bottom());
        }
    };
    REQUIRE(menu.open(style.text(), {}, labels, style.nativeTopics(labels.title, labels.topics)));
    REQUIRE(menu.topics().definition().items.size() == 7);
    checkWhole(menu.topics());
    // Asked for, the list is laid out as his topics are, under his own heading.
    const auto row = menu.topics().itemArea(6);
    MenuInput click;
    click.pointer = Vec2{row.x + row.width / 2, row.y + row.height / 2};
    click.pointerPressed = true;
    const auto asked = menu.update(device, click, 1);
    REQUIRE(asked.kind == HintMenuEvent::Kind::Asked);
    REQUIRE(asked.topic == kGatewayTopic);
    std::vector<MenuItem> places{
        {std::string{words[1]}, 0}, {std::string{words[2]}, 1}, {std::string{words[3]}, 2}};
    places[1].enabled = false; // the N64's own world, where the party stands
    REQUIRE(menu.choose(places));
    const OptionMenu& list = menu.page();
    CHECK(list.definition().backdrop == "TEXTSCROLL");
    CHECK_FALSE(list.definition().showCursor);
    checkWhole(list);
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    device.draws.clear();
    menu.draw(canvas, style.text());
    canvas.end();
    // The scroll, its heading in the title font, and the rows in the body font: the greyed one
    // at half strength.
    REQUIRE(device.draws.size() == 3);
    CHECK(device.draws[0].texture == art.textures.backdrop);
    CHECK(device.draws[1].vertices.front().color == Color::rgba(0, 0, 132));
    bool greyed = false;
    for (const ImmediateVertex& vertex : device.draws[2].vertices) {
        greyed = greyed || vertex.color == Color::rgba(0, 0, 0, 127);
    }
    CHECK(greyed);
}
