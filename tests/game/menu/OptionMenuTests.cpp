#include <cmath>
#include <cstdint>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/io/File.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/ModelSprite.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/menu/MenuInput.h"
#include "game/menu/OptionMenu.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** Every upper-case letter is 8 pixels wide on a 10 pixel line. */
BitmapFont wideFont() {
    std::vector<BitmapGlyph> glyphs;
    for (std::int32_t c = 'A'; c <= 'Z'; ++c) {
        glyphs.push_back({c, 8, 0, 0});
    }
    return BitmapFont::fromGlyphs(10, 4, std::move(glyphs));
}

MenuDefinition threeItems() {
    MenuDefinition menu;
    menu.items = {{"AA", 1}, {"BBBB", 2}, {"C", 3}};
    return menu;
}

MenuInput pressed(bool up, bool down, bool select, bool back, bool start = false) {
    MenuInput input;
    input.up = up;
    input.down = down;
    input.select = select;
    input.back = back;
    input.start = start;
    return input;
}

struct Fixture {
    BitmapFont font = wideFont();
    test::FakeTexture sheet{64, 64};
    TextPainter painter;
    OptionMenu menu;

    Fixture() { painter.setFont(&font, &sheet); }
};

TEST_CASE("a menu lays its column out around the screen centre", "[game][menu]") {
    Fixture f;
    f.menu.open(threeItems(), f.painter, MenuScreen{});
    REQUIRE(f.menu.isOpen());
    REQUIRE(f.menu.lineHeight() == 10);
    REQUIRE(f.menu.columnHeight() == 30);
    REQUIRE(f.menu.columnWidth() == 32);
    REQUIRE(f.menu.itemY(0) == 192 - 15);
    REQUIRE(f.menu.itemY(2) == 192 - 15 + 20);
    REQUIRE(f.menu.columnX() == 256 - 16);
    REQUIRE(f.menu.iconY() == f.menu.itemY(0) + 5);

    MenuDefinition placed = threeItems();
    placed.x = 128;
    placed.y = 300;
    placed.items[0].extraSpacing = 6;
    f.menu.open(placed, f.painter, MenuScreen{}, 5);
    REQUIRE(f.menu.selection() == 2);
    REQUIRE(f.menu.columnX() == 128);
    REQUIRE(f.menu.itemY(1) == 316);
    REQUIRE(f.menu.backdropArea() == Rect{256.0f - 80.0f, 192.0f - 82.0f, 160.0f, 164.0f});
}

TEST_CASE("navigation wraps and reports choices and backing out", "[game][menu]") {
    Fixture f;
    f.menu.open(threeItems(), f.painter, MenuScreen{});
    REQUIRE(f.menu.update(pressed(false, true, false, false), 1).action == MenuAction::Moved);
    REQUIRE(f.menu.selection() == 1);
    REQUIRE(f.menu.update(pressed(true, false, false, false), 1).action == MenuAction::Moved);
    REQUIRE(f.menu.update(pressed(true, false, false, false), 1).action == MenuAction::Moved);
    REQUIRE(f.menu.selection() == 2);
    REQUIRE(f.menu.update(pressed(false, true, false, false), 1).action == MenuAction::Moved);
    REQUIRE(f.menu.selection() == 0);
    REQUIRE(f.menu.update(MenuInput{}, 1).action == MenuAction::None);
    REQUIRE(f.menu.time() == 5);

    const MenuEvent choice = f.menu.update(pressed(false, false, true, false), 1);
    REQUIRE(choice.action == MenuAction::Choice);
    REQUIRE(choice.code == 1);
    REQUIRE(f.menu.update(pressed(false, false, false, true), 1).action == MenuAction::Back);
    REQUIRE(f.menu.update(pressed(false, false, false, false, true), 1).action == MenuAction::None);

    MenuDefinition title = threeItems();
    title.startSelects = true;
    f.menu.open(title, f.painter, MenuScreen{}, 1);
    const MenuEvent started = f.menu.update(pressed(false, false, false, false, true), 1);
    REQUIRE(started.action == MenuAction::Choice);
    REQUIRE(started.code == 2);
}

TEST_CASE("fading menus close over thirty ticks and others at once", "[game][menu]") {
    Fixture f;
    f.menu.open(threeItems(), f.painter, MenuScreen{});
    REQUIRE(f.menu.fadeOpacity() == 255);
    f.menu.close();
    REQUIRE_FALSE(f.menu.isOpen());

    MenuDefinition fading = threeItems();
    fading.fades = true;
    f.menu.open(fading, f.painter, MenuScreen{});
    REQUIRE(f.menu.fadeOpacity() == 0);
    f.menu.update(MenuInput{}, 15);
    REQUIRE(f.menu.fadeOpacity() == 128);
    f.menu.update(MenuInput{}, 15);
    REQUIRE(f.menu.fadeOpacity() == 255);
    f.menu.close();
    REQUIRE(f.menu.isOpen());
    REQUIRE(f.menu.closing());
    REQUIRE(f.menu.update(pressed(false, true, false, false), 10).action == MenuAction::None);
    REQUIRE(f.menu.selection() == 0);
    REQUIRE(f.menu.fadeOpacity() < 255);
    REQUIRE(f.menu.update(MenuInput{}, 10).action == MenuAction::None);
    REQUIRE(f.menu.update(MenuInput{}, 10).action == MenuAction::Closed);
    REQUIRE_FALSE(f.menu.isOpen());
}

TEST_CASE("the selection icon glides between items", "[game][menu]") {
    Fixture f;
    f.menu.open(threeItems(), f.painter, MenuScreen{});
    const std::int32_t first = f.menu.itemY(0) + 5;
    const std::int32_t second = f.menu.itemY(1) + 5;
    REQUIRE(f.menu.iconY() == first);
    f.menu.update(pressed(false, true, false, false), 1);
    REQUIRE(f.menu.iconY() == first);
    f.menu.update(MenuInput{}, 1);
    REQUIRE(f.menu.iconY() == first);
    f.menu.update(MenuInput{}, 1);
    REQUIRE(f.menu.iconY() > first);
    REQUIRE(f.menu.iconY() < second);
    for (int i = 0; i < 20; ++i) {
        f.menu.update(MenuInput{}, 1);
    }
    REQUIRE(f.menu.iconY() == second);
}

TEST_CASE("the glow pulse rises and falls with a hold between", "[game][menu]") {
    REQUIRE(pulseOpacity(0, 40, 5) == 128);
    REQUIRE(pulseOpacity(40, 40, 5) == 255);
    REQUIRE(pulseOpacity(80, 40, 5) == 128);
    REQUIRE(pulseOpacity(83, 40, 5) == 128);
    REQUIRE(pulseOpacity(85, 40, 5) == 128);
    REQUIRE(pulseOpacity(20, 40, 5) > 128);
    REQUIRE(pulseOpacity(20, 40, 5) < 255);
}

TEST_CASE("drawing emits the glow, the labels, the icon and the prompts", "[game][menu]") {
    Fixture f;
    test::FakeRenderDevice device;
    const test::FakeTexture glow{64, 64};
    const test::FakeTexture arrows{32, 32};
    const test::FakeTexture backdrop{16, 16};
    MenuTextures textures;
    textures.font = &f.sheet;
    textures.glow = &glow;
    textures.arrows = &arrows;
    textures.backdrop = &backdrop;

    MenuDefinition definition = threeItems();
    definition.title = "OPTIONS";
    definition.backdrop = "SCROLL";
    definition.prompts = true;
    f.menu.open(definition, f.painter, MenuScreen{});
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    f.menu.draw(canvas, f.painter, textures);
    canvas.end();

    bool sawGlow = false;
    bool sawArrow = false;
    bool sawBackdrop = false;
    for (const test::RecordedDraw& draw : device.draws) {
        sawGlow = sawGlow || draw.texture == &glow;
        sawArrow = sawArrow || draw.texture == &arrows;
        sawBackdrop = sawBackdrop || draw.texture == &backdrop;
    }
    REQUIRE(sawGlow);
    REQUIRE(sawArrow);
    REQUIRE(sawBackdrop);
    REQUIRE(device.draws.front().texture == &backdrop);
}

TEST_CASE("a menu writes its body in ink and centres a lone prompt", "[game][menu]") {
    Fixture f;
    test::FakeRenderDevice device;
    const test::FakeTexture backdrop{16, 16};
    MenuTextures textures;
    textures.font = &f.sheet;
    textures.backdrop = &backdrop;
    MenuDefinition page;
    page.backdrop = "SCROLL";
    page.body = {"AB\nC", "D"};
    page.bodyY = 100;
    page.bodyGap = 6;
    page.colors.off = Color::rgba(92, 26, 3);
    page.prompts = true;
    page.backLabel = "BACK";
    page.promptY = 300;
    f.menu.open(page, f.painter, MenuScreen{});
    REQUIRE(f.menu.bodyTop() == 100);
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    f.menu.draw(canvas, f.painter, textures);
    canvas.end();
    // Body glyphs in ink: "AB" centred on the screen at the top, "D" two lines and a gap on.
    std::vector<Vec3> ink;
    float promptLeft = 1000.0f;
    float promptRight = 0.0f;
    for (const test::RecordedDraw& draw : device.draws) {
        for (const ImmediateVertex& vertex : draw.vertices) {
            if (vertex.color.r == 92 && vertex.color.g == 26) {
                ink.push_back(vertex.position);
            } else if (vertex.position.y >= 300.0f && draw.texture == &f.sheet) {
                promptLeft = std::min(promptLeft, vertex.position.x);
                promptRight = std::max(promptRight, vertex.position.x);
            }
        }
    }
    REQUIRE_FALSE(ink.empty());
    float top = 1000.0f;
    float bottom = 0.0f;
    for (const Vec3& position : ink) {
        top = std::min(top, position.y);
        bottom = std::max(bottom, position.y);
    }
    REQUIRE(std::abs(top - 100.0f) <= 1.0f);
    REQUIRE(std::abs(bottom - (100.0f + 20.0f + 6.0f + 10.0f)) <= 1.0f);   // the last line's foot
    REQUIRE(std::abs((promptLeft + promptRight) / 2.0f - 256.0f) <= 1.0f); // alone, mid-row

    // A lone centred passage sits about the column's middle.
    page.bodyY = -1;
    page.body = {"AB\nC"};
    f.menu.open(page, f.painter, MenuScreen{});
    REQUIRE(f.menu.bodyTop() == 192 - 10);
}

TEST_CASE("a bound arrow model replaces the flat arrow glyph", "[game][menu]") {
    const auto dir = test::scratchDirectory("menu-arrow");
    std::filesystem::create_directories(dir / "models");
    std::filesystem::create_directories(dir / "textures");
    writeTextFile(dir / "models/000_ICON_ARROWFR.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(dir / "objects.json", R"({"objects": [
  {"index": 0, "name": "ICON_ARROWFR", "file": "models/000_ICON_ARROWFR.obj"}]})");
    writeFile(dir / "textures/000_T.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({"defs": [], "bitmaps": [
  {"index": 0, "name": "T", "file": "textures/000_T.png", "width": 2, "height": 2}]})");
    writeTextFile(dir / "animations.json", R"({"trees": [{"name": "ICON_ARROW", "prefix": "",
  "sequences": [], "nodes": [{"name": "FR", "object": "ICON_ARROWFR", "type": 1, "flags": 0,
  "objectFlags": 0, "parent": -1, "position": [0, 0, 0]}]}]})");
    ModelSet models;
    TextureSet sheets;
    AnimationSet trees;
    REQUIRE(models.load(dir));
    REQUIRE(sheets.load(dir));
    REQUIRE(trees.load(dir));
    test::FakeRenderDevice device;
    ModelSprite arrow;
    REQUIRE(arrow.bind(trees.tree(0), models, sheets, device));

    Fixture f;
    const test::FakeTexture arrows{32, 32};
    MenuTextures textures;
    textures.font = &f.sheet;
    textures.arrows = &arrows;
    textures.icon = &arrow;
    f.menu.open(threeItems(), f.painter, MenuScreen{});
    REQUIRE(f.menu.iconAngle() == 0.0f);
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    f.menu.draw(canvas, f.painter, textures);
    canvas.end();
    bool sawGlyph = false;
    bool sawModel = false;
    for (const test::RecordedDraw& draw : device.draws) {
        sawGlyph = sawGlyph || draw.texture == &arrows;
        sawModel = sawModel || (draw.vertices.size() == 3 && draw.texture != &f.sheet);
    }
    REQUIRE_FALSE(sawGlyph);
    REQUIRE(sawModel);

    f.menu.update(pressed(false, true, false, false), 1);
    REQUIRE(f.menu.iconAngle() == kPi);
    f.menu.update(MenuInput{}, 1);
    f.menu.update(MenuInput{}, 5);
    REQUIRE(f.menu.iconAngle() > kPi);
    REQUIRE(f.menu.iconAngle() < 2.0f * kPi);
}

TEST_CASE("the selection arrow is sized by the camera it was modelled for", "[game][menu]") {
    // 512 wide at a 60 degree horizontal field of view: focal length 443.4, scale 0.05 at 1.1.
    REQUIRE(std::abs(OptionMenu::iconPixelsPerUnit(MenuScreen{}) - 20.155f) < 0.01f);
    MenuScreen wide;
    wide.width = 1024;
    REQUIRE(std::abs(OptionMenu::iconPixelsPerUnit(wide) - 40.31f) < 0.01f);

    Fixture f;
    f.menu.open(threeItems(), f.painter, wide);
    REQUIRE(f.menu.iconScale() == OptionMenu::iconPixelsPerUnit(wide));
    // A backdrop without a placement centres on the screen, whatever its width.
    REQUIRE(f.menu.backdropArea().x == 1024.0f / 2.0f - f.menu.backdropArea().width / 2.0f);
}

TEST_CASE("releasing the backdrop keeps the text fading without it", "[game][menu]") {
    Fixture f;
    MenuDefinition definition = threeItems();
    definition.fades = true;
    definition.backdrop = "SCROLL";
    definition.backdropWidth = 100;
    definition.backdropHeight = 100;
    f.menu.open(definition, f.painter, MenuScreen{});
    f.menu.update(pressed(false, false, false, false), OptionMenu::kFadeTicks);
    test::FakeTexture backdrop{8, 8};
    MenuTextures textures;
    textures.font = &f.sheet;
    textures.backdrop = &backdrop;
    textures.arrows = &f.sheet;

    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    f.menu.draw(canvas, f.painter, textures);
    canvas.end();
    const auto withBackdrop = device.draws.size();
    REQUIRE(device.draws[0].texture == &backdrop);

    f.menu.releaseBackdrop();
    f.menu.close();
    REQUIRE(f.menu.backdropReleased());
    REQUIRE(f.menu.closing());
    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    f.menu.draw(canvas, f.painter, textures);
    canvas.end();
    REQUIRE(device.draws.size() < withBackdrop);
    for (const auto& draw : device.draws) {
        REQUIRE(draw.texture != &backdrop);
    }
}

} // namespace
