#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/ModelSprite.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/widgets/MenuInput.h"
#include "game/widgets/OptionMenu.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** Every upper-case letter is 8 pixels wide on a 10 pixel line. */
BitmapFont wideFont() {
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = 'A'; c <= 'Z'; ++c) {
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

TEST_CASE("menu pointer targets follow rendered text and do not steal keyboard focus",
          "[game][menu][mouse]") {
    Fixture f;
    auto definition = threeItems();
    definition.items[1].enabled = false;
    f.menu.open(definition, f.painter, {});
    const auto area = f.menu.itemArea(2);
    CHECK(area.x == 252);
    CHECK(area.width == 8);
    MenuInput input;
    input.pointer = Vec2{area.x + 1, area.y + 1};
    CHECK(f.menu.update(input, 1).action == MenuAction::Moved);
    CHECK(f.menu.selection() == 2);
    input.up = true;
    f.menu.update(input, 1);
    CHECK(f.menu.selection() == 0);
    input.up = false;
    f.menu.update(input, 1);
    CHECK(f.menu.selection() == 0);
    input.pointerPressed = true;
    CHECK(f.menu.update(input, 1).code == 3);
    const auto disabled = f.menu.itemArea(1);
    input.pointer = Vec2{disabled.x + 1, disabled.y + 1};
    CHECK(f.menu.update(input, 1).action == MenuAction::None);
    input.pointer = Vec2{0, 0};
    CHECK(f.menu.update(input, 1).action == MenuAction::None);
    input.pointer = Vec2{area.x + 1, area.y + 1};
    f.menu.close();
    CHECK(f.menu.update(input, 1).action == MenuAction::None);
}

TEST_CASE("menu pointer respects explicit action rows and value arrow direction",
          "[game][menu][mouse]") {
    Fixture f;
    MenuDefinition definition;
    definition.items = {{"AA", 1}, {"BB", 2}, {"CC", 3}};
    definition.items[0].value = "ON";
    definition.valueX = 150;
    definition.valueWidth = 24;
    definition.itemPositions = {{40, 40}, {40, 100}, {120, 100}};
    f.menu.open(definition, f.painter, {});
    MenuInput input;
    input.pointerPressed = true;
    input.pointer = Vec2{145, 45};
    CHECK(f.menu.update(input, 1).direction == -1);
    input.pointer = Vec2{182, 45};
    CHECK(f.menu.update(input, 1).direction == 1);
    input.pointer = Vec2{121, 101};
    CHECK(f.menu.update(input, 1).code == 3);
    input.pointer = Vec2{90, 101};
    CHECK(f.menu.update(input, 1).action == MenuAction::None);
}

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

TEST_CASE("native menu skins can disable selection glow without hiding the selected label",
          "[game][menu][imported-menu]") {
    Fixture f;
    auto definition = threeItems();
    SECTION("ordinary row") {}
    SECTION("alternate row") {
        definition.items[0].alternate = "BB";
    }
    definition.colors.hi = Color::rgba(0, 0, 0, 0);
    f.menu.open(definition, f.painter, {});
    f.menu.update({}, 120);
    test::FakeRenderDevice device;
    test::FakeTexture glow{64, 64};
    MenuTextures textures;
    textures.glow = &glow;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    f.menu.draw(canvas, f.painter, textures);
    canvas.end();
    CHECK(std::ranges::none_of(device.draws,
                               [&](const auto& draw) { return draw.texture == &glow; }));
    CHECK(std::ranges::any_of(device.draws,
                              [&](const auto& draw) { return draw.texture == &f.sheet; }));
}

TEST_CASE("two-column choices draw arrows and gray disabled values without a cursor",
          "[game][menu][graphics]") {
    Fixture f;
    MenuDefinition definition;
    definition.items = {{"AA", 0}, {"BB", 1, 0, false}};
    definition.items[0].value = "ON";
    definition.items[1].value = "OFF";
    definition.itemPositions = {{64, 132}, {64, 160}};
    definition.valueX = 200;
    definition.valueWidth = 32;
    definition.showCursor = false;
    f.menu.open(definition, f.painter, {});
    CHECK(f.menu.columnRight() == 200 + 32 + 11); // the value column and its arrow
    test::FakeRenderDevice device;
    test::FakeTexture cursor{32, 32};
    MenuTextures textures;
    textures.font = &f.sheet;
    textures.arrows = &cursor;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    f.menu.draw(canvas, f.painter, textures);
    canvas.end();
    s32 arrows = 0;
    bool grayLabel = false;
    bool grayValue = false;
    for (const auto& draw : device.draws) {
        CHECK(draw.texture != &cursor);
        if (draw.vertices.size() == 3 && draw.texture == &device.whiteTexture()) {
            ++arrows;
            CHECK(test::maxCorner(draw).y < 160);
        }
        for (const auto& vertex : draw.vertices) {
            if (vertex.color == Color::rgba(128, 128, 128) &&
                vertex.position.y == 160 + TextPainter::kCellInset) {
                grayLabel |= vertex.position.x == 64 + TextPainter::kCellInset;
                grayValue |= vertex.position.x == 200 + TextPainter::kCellInset;
            }
        }
    }
    CHECK(arrows == 2);
    CHECK(grayLabel);
    CHECK(grayValue);
}

TEST_CASE("standalone choice rows draw and hit-test their own centered column",
          "[game][menu][mouse]") {
    Fixture f;
    MenuDefinition definition;
    definition.items = {{"AA", 0}, {"", 1}};
    definition.items[0].value = "ON";
    definition.items[1].value = "PAGE";
    definition.items[1].valueColumn = MenuValueColumn{240, 32};
    definition.itemPositions = {{64, 100}, {240, 140}};
    definition.valueX = 350;
    definition.valueWidth = 40;
    definition.showCursor = false;
    f.menu.open(definition, f.painter, {});
    CHECK(f.menu.itemArea(1) == Rect{229, 140, 54, 10});
    test::FakeRenderDevice device;
    MenuTextures textures;
    textures.font = &f.sheet;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    f.menu.draw(canvas, f.painter, textures);
    canvas.end();
    std::vector<f32> arrowTips;
    for (const auto& draw : device.draws) {
        if (draw.vertices.size() == 3 && draw.texture == &device.whiteTexture()) {
            arrowTips.push_back(draw.vertices.front().position.x);
        }
    }
    CHECK(arrowTips == std::vector<f32>{339, 401, 229, 283});
    MenuInput click;
    click.pointerPressed = true;
    click.pointer = Vec2{232, 145};
    auto event = f.menu.update(click, 1);
    CHECK(event.code == 1);
    CHECK(event.direction == -1);
    click.pointer = Vec2{280, 145};
    event = f.menu.update(click, 1);
    CHECK(event.code == 1);
    CHECK(event.direction == 1);
}

TEST_CASE("a long column scrolls behind its window, by the keys, the wheel and a tail row",
          "[game][menu][scroll-menu]") {
    Fixture f;
    MenuDefinition definition;
    for (s32 i = 0; i < 8; ++i) {
        definition.items.push_back({std::string(1, static_cast<char>('A' + i)), i});
    }
    definition.items.push_back({"APPLY", 100});
    definition.items.push_back({"RESET", 101});
    definition.tailPositions = {Vec2{100, 300}, Vec2{200, 300}};
    definition.visibleRows = 3;
    definition.x = 10;
    definition.y = 20;
    f.menu.open(definition, f.painter, MenuScreen{});
    CHECK(f.menu.windowRows() == 3);
    CHECK(f.menu.scroll() == 0);
    CHECK(f.menu.columnHeight() == 3 * f.menu.lineHeight());
    for (usize i = 0; i < 10; ++i) {
        CHECK(f.menu.shown(i) == (i < 3 || i >= 8));
    }
    CHECK(f.menu.itemY(1) == f.menu.itemY(0) + f.menu.lineHeight());
    CHECK(f.menu.itemY(8) == 300);
    CHECK(f.menu.itemX(9) == 200);
    CHECK(f.menu.itemArea(5).width == 0); // out of the window: nothing to point at
    // Down past the window scrolls it; up at its top scrolls back.
    for (s32 i = 0; i < 4; ++i) {
        f.menu.update(pressed(false, true, false, false), 1);
    }
    CHECK(f.menu.selection() == 4);
    CHECK(f.menu.scroll() == 2);
    CHECK(f.menu.shown(4));
    CHECK_FALSE(f.menu.shown(1));
    CHECK(f.menu.itemY(2) == 20); // the window's first row stands where the column starts
    CHECK(f.menu.itemY(1) == 20 - f.menu.lineHeight());
    f.menu.focus(0);
    CHECK(f.menu.scroll() == 0);
    // The wheel scrolls the window a row a notch and stops at its ends; the selection stays.
    MenuInput wheel;
    wheel.pointerScroll = -2.0f;
    CHECK(f.menu.update(wheel, 1).action == MenuAction::Moved);
    CHECK(f.menu.selection() == 0);
    CHECK(f.menu.scroll() == 2);
    CHECK_FALSE(f.menu.shown(0)); // out of the window until a key brings it back
    wheel.pointerScroll = -1.0f;
    f.menu.update(wheel, 1);
    CHECK(f.menu.selection() == 0);
    CHECK(f.menu.scroll() == 3);
    wheel.pointerScroll = 1.0f;
    f.menu.update(wheel, 1);
    CHECK(f.menu.scroll() == 2);
    f.menu.update(pressed(false, true, false, false), 1); // a key: the window finds the selection
    CHECK(f.menu.selection() == 1);
    CHECK(f.menu.scroll() == 1); // by the least it can move
    f.menu.focus(7);
    CHECK(f.menu.scroll() == 5);
    wheel.pointerScroll = -5.0f;
    CHECK(f.menu.update(wheel, 1).action == MenuAction::None); // the column's end
    CHECK(f.menu.selection() == 7);
    // Wrapping from the last tail item comes back to the column's first row.
    f.menu.update(pressed(false, true, false, false), 1);
    CHECK(f.menu.selection() == 8);
    f.menu.update(pressed(false, true, false, false), 1);
    f.menu.update(pressed(false, true, false, false), 1);
    CHECK(f.menu.selection() == 0);
    CHECK(f.menu.scroll() == 0);
    // A pointer finds only what is in the window.
    MenuInput point;
    point.pointer = Vec2{f.menu.itemArea(1).x + 1, f.menu.itemArea(1).y + 1};
    point.pointerPressed = true;
    CHECK(f.menu.update(point, 1).code == 1);
    f.menu.focus(7);
    point.pointer = Vec2{f.menu.itemArea(7).x + 1, f.menu.itemArea(7).y + 1};
    CHECK(f.menu.update(point, 1).code == 7);
    CHECK(f.menu.pointerChoice(point) == 7);
    // Opened again with its window where it was, the menu keeps it as long as the selection
    // stays inside; a window the selection is outside of is brought to it.
    f.menu.open(definition, f.painter, MenuScreen{}, 3, 2);
    CHECK(f.menu.selection() == 3);
    CHECK(f.menu.scroll() == 2);
    f.menu.open(definition, f.painter, MenuScreen{}, 0, 5);
    CHECK(f.menu.scroll() == 0);
    f.menu.open(definition, f.painter, MenuScreen{}, 7, 9);
    CHECK(f.menu.scroll() == 5);
    // The bar and its marks stand at the column's right hand, past its widest row: the
    // column's own, a letter wide; the tail's longer labels stand apart from it.
    CHECK(f.menu.columnRight() == f.menu.columnX() + 8);
    f.menu.open(definition, f.painter, MenuScreen{}, 3, 2);
    test::FakeRenderDevice device;
    MenuTextures textures;
    textures.font = &f.sheet;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    f.menu.draw(canvas, f.painter, textures);
    canvas.end();
    s32 marks = 0;
    s32 bars = 0;
    for (const auto& draw : device.draws) {
        if (draw.texture != &device.whiteTexture() ||
            test::minCorner(draw).x <= static_cast<f32>(f.menu.columnRight())) {
            continue;
        }
        marks += draw.vertices.size() == 3 ? 1 : 0;
        bars += draw.vertices.size() > 3 ? static_cast<s32>(draw.vertices.size()) : 0;
    }
    CHECK(marks == 2); // rows wait above and below
    CHECK(bars >= 8);  // the track and its thumb, four corners each
}

TEST_CASE("the confirmation dialog is the retail yes/no parchment", "[game][menu]") {
    const MenuDefinition dialog = MenuDefinition::confirmation("QUIT?", "NO", "YES");
    CHECK(dialog.title == "QUIT?");
    CHECK(dialog.titleScale == 1.2f);
    CHECK(dialog.x == -256);
    CHECK(dialog.backdrop == "SCROLL_A");
    CHECK(dialog.backdropX == -1);
    CHECK(dialog.backdropY == 64);
    CHECK(dialog.backdropWidth == 320);
    CHECK(dialog.backdropHeight == 220);
    CHECK(dialog.parchmentFont);
    CHECK(dialog.fades);
    CHECK_FALSE(dialog.prompts);
    CHECK(dialog.playerLabel.empty());
    CHECK(dialog.burn.empty());
    REQUIRE(dialog.items.size() == 2);
    CHECK(dialog.items[0].text == "NO");
    CHECK(dialog.items[0].code == 0);
    CHECK(dialog.items[1].text == "YES");
    CHECK(dialog.items[1].code == 1);
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

TEST_CASE("a menu with no enabled entries cannot confirm a disabled choice", "[game][menu]") {
    Fixture f;
    MenuDefinition definition;
    definition.items = {{"EMPTY", 0, 0, false}};
    f.menu.open(definition, f.painter, {});
    CHECK(f.menu.update(pressed(false, false, true, false), 1).action == MenuAction::None);
    CHECK(f.menu.update(pressed(false, false, false, true), 1).action == MenuAction::Back);
}

TEST_CASE("a menu with no row to choose lights none, its arrow still at the selection",
          "[game][menu]") {
    Fixture f;
    test::FakeRenderDevice device;
    const test::FakeTexture glow{64, 64};
    const test::FakeTexture arrows{32, 32};
    MenuTextures textures;
    textures.font = &f.sheet;
    textures.glow = &glow;
    textures.arrows = &arrows;
    MenuDefinition definition = threeItems();
    const auto drawn = [&] {
        f.menu.open(definition, f.painter, MenuScreen{});
        device.draws.clear();
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        f.menu.draw(canvas, f.painter, textures);
        canvas.end();
        bool glowing = false;
        bool pointing = false;
        for (const test::RecordedDraw& draw : device.draws) {
            glowing = glowing || draw.texture == &glow;
            pointing = pointing || draw.texture == &arrows;
        }
        return std::pair{glowing, pointing};
    };
    definition.items[0].enabled = false;
    definition.items[2].enabled = false;
    CHECK(drawn() == std::pair{true, true}); // the one row left to choose is lit and pointed at
    // show_optmenu (0x80072AA4) lights the selected row only when its value is not negative, and
    // draws its arrow at the selection whatever the row.
    definition.items[1].enabled = false;
    CHECK(drawn() == std::pair{false, true});
    // Every row is drawn greyed, the one the selection rests on among them.
    CHECK(f.menu.selection() == 0);
    usize letters = 0;
    for (const test::RecordedDraw& draw : device.draws) {
        if (draw.texture != &f.sheet) {
            continue;
        }
        for (const ImmediateVertex& vertex : draw.vertices) {
            CHECK(vertex.color == definition.colors.off.withAlpha(127));
            ++letters;
        }
    }
    CHECK(letters > 0);
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

TEST_CASE("a menu can fade its text on exit without changing how it first appears",
          "[game][menu][pause-dismiss]") {
    Fixture f;
    f.menu.open(threeItems(), f.painter, {});
    CHECK(f.menu.fadeOpacity() == 255);
    f.menu.releaseBackdrop();
    f.menu.closeWithFade();
    REQUIRE(f.menu.closing());
    CHECK(f.menu.fadeOpacity() > 0);
    CHECK(f.menu.fadeOpacity() < 255);
    CHECK(f.menu.update(pressed(false, true, true, false), 14).action == MenuAction::None);
    CHECK(f.menu.selection() == 0);
    CHECK(f.menu.fadeOpacity() == 128);
    CHECK(f.menu.update({}, 15).action == MenuAction::Closed);
    CHECK_FALSE(f.menu.isOpen());
    f.menu.open(threeItems(), f.painter, {});
    CHECK_FALSE(f.menu.backdropReleased());
    CHECK_FALSE(f.menu.closing());
    CHECK(f.menu.fadeOpacity() == 255);
}

TEST_CASE("the selection icon glides between items", "[game][menu]") {
    Fixture f;
    f.menu.open(threeItems(), f.painter, MenuScreen{});
    const s32 first = f.menu.itemY(0) + 5;
    const s32 second = f.menu.itemY(1) + 5;
    REQUIRE(f.menu.iconY() == first);
    f.menu.update(pressed(false, true, false, false), 1);
    REQUIRE(f.menu.iconY() == first);
    f.menu.update(MenuInput{}, 1);
    REQUIRE(f.menu.iconY() == first);
    f.menu.update(MenuInput{}, 1);
    REQUIRE(f.menu.iconY() > first);
    REQUIRE(f.menu.iconY() < second);
    for (s32 i = 0; i < 20; ++i) {
        f.menu.update(MenuInput{}, 1);
    }
    REQUIRE(f.menu.iconY() == second);
}

TEST_CASE("the menu icon retains the native rotation on both glide boundaries",
          "[game][menu][menu-icon-timing]") {
    // show_optmenu (GC 0x80073240..0x800732E4) starts the timer at one without
    // rotating, then draws the incremented timer even when it reaches/passes 15.
    // The following update settles the icon. Position and rotation share that phase.
    for (const s32 ticks : {1, 2, 5, 20}) {
        for (const usize selected : {usize{1}, usize{2}}) {
            CAPTURE(ticks, selected);
            Fixture f;
            f.menu.open(threeItems(), f.painter, {});
            const auto first = f.menu.iconY();
            const auto target = f.menu.itemY(selected) + f.menu.lineHeight() / 2;
            const auto base = selected == 1 ? kPi : 0.0f;
            f.menu.focus(selected);
            f.menu.update({}, ticks);
            CHECK(f.menu.iconY() == first);
            CHECK(f.menu.iconAngle() == base);
            s32 timer = 1;
            while (timer < OptionMenu::kIconGlideTicks) {
                timer += ticks;
                f.menu.update({}, ticks);
                CAPTURE(timer);
                CHECK(f.menu.iconY() ==
                      first + (target - first) * timer / OptionMenu::kIconGlideTicks);
                const auto angle =
                    base + kPi * static_cast<f32>(timer) / OptionMenu::kIconGlideTicks;
                CHECK(std::abs(f.menu.iconAngle() - angle) < 0.00001f);
            }
            f.menu.update({}, ticks);
            CHECK(f.menu.iconY() == target);
            CHECK(f.menu.iconAngle() == base);
        }
    }

    Fixture f;
    f.menu.open(threeItems(), f.painter, {});
    f.menu.focus(1);
    f.menu.update({}, 1);
    f.menu.update({}, 7);
    REQUIRE(f.menu.iconAngle() > kPi);
    f.menu.close();
    f.menu.open(threeItems(), f.painter, {}, 1);
    CHECK(f.menu.iconY() == f.menu.itemY(1) + f.menu.lineHeight() / 2);
    CHECK(f.menu.iconAngle() == kPi);
}

TEST_CASE("ornate menu glyphs retain their native entry and dismissal frames",
          "[game][menu][menu-font-timing]") {
    // show_optmenu (GC 0x80072AA4) deliberately changes FONT32GAR sheets before
    // the labels resolve. This is authored animation, not damaged font geometry.
    Fixture f;
    const test::FakeTexture glow{64, 64};
    const test::FakeTexture parchment{64, 64};
    const std::array frames{test::FakeTexture{64, 64}, test::FakeTexture{64, 64},
                            test::FakeTexture{64, 64}, test::FakeTexture{64, 64},
                            test::FakeTexture{64, 64}, test::FakeTexture{64, 64}};
    MenuTextures textures;
    textures.font = &f.sheet;
    textures.glow = &glow;
    textures.parchment = &parchment;
    for (usize i = 0; i < frames.size(); ++i) {
        textures.garamond[i] = &frames[i];
    }
    auto definition = threeItems();
    definition.fades = true;
    definition.parchmentFont = true;
    definition.garamondIntro = true;
    definition.showCursor = false;
    for (const bool closing : {false, true}) {
        for (s32 tick = closing ? 1 : 0; tick < OptionMenu::kFadeTicks; ++tick) {
            CAPTURE(closing, tick);
            f.menu.open(definition, f.painter, {});
            if (closing) {
                f.menu.update({}, OptionMenu::kFadeTicks);
                f.menu.close();
                f.menu.update({}, tick - 1);
            } else {
                f.menu.update({}, tick);
            }
            const auto frame = closing ? 6 - tick * 6 / OptionMenu::kFadeTicks : (tick - 10) / 2;
            const Texture* selectedSheet =
                frame < 6 ? &frames[static_cast<usize>(std::max(0, frame))] : &f.sheet;
            const Texture* otherSheet = frame < 6 ? selectedSheet : &parchment;
            test::FakeRenderDevice device;
            Canvas canvas;
            canvas.begin(device, Mat4{1});
            f.menu.draw(canvas, f.painter, textures);
            canvas.end();
            REQUIRE(device.draws.size() == (selectedSheet == otherSheet ? 2 : 3));
            CHECK(device.draws[0].texture == &glow);
            CHECK(device.draws[1].texture == selectedSheet);
            CHECK(device.draws.back().texture == otherSheet);
        }
    }
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

TEST_CASE("selection glow combines its pulse with the native menu fade",
          "[game][menu][menu-fade]") {
    // show_optmenu (GC 0x80072AA4) adds the pulse's transparency to the
    // menu's transparency before clamping. It does not pick the dimmer opacity.
    struct Sample {
        s32 time;
        s32 closingTime;
        u8 labelAlpha;
        u8 glowAlpha;
    };
    constexpr std::array kSamples{
        Sample{0, 0, 0, 0},      Sample{10, 0, 85, 0},    Sample{20, 0, 170, 107},
        Sample{30, 0, 255, 224}, Sample{40, 0, 255, 255}, Sample{80, 0, 255, 128},
        Sample{60, 1, 247, 184}, Sample{60, 15, 128, 65}, Sample{60, 29, 9, 0},
    };
    for (const auto& sample : kSamples) {
        for (const s32 ticks : {1, 2}) {
            for (const s32 row : {0, 1, 2}) {
                CAPTURE(sample.time, sample.closingTime, ticks, row);
                Fixture f;
                auto definition = threeItems();
                definition.fades = true;
                if (row == 1) {
                    definition.items[0].alternate = "BB";
                    definition.items[0].markedPart = 2;
                } else if (row == 2) {
                    definition.items[0].value = "CC";
                    definition.valueX = 300;
                }
                f.menu.open(definition, f.painter, {});
                for (s32 elapsed = 0; elapsed < sample.time; elapsed += ticks) {
                    f.menu.update({}, std::min(ticks, sample.time - elapsed));
                }
                if (sample.closingTime > 0) {
                    f.menu.close();
                    for (s32 elapsed = 1; elapsed < sample.closingTime; elapsed += ticks) {
                        f.menu.update({}, std::min(ticks, sample.closingTime - elapsed));
                    }
                }
                REQUIRE(f.menu.isOpen());
                CHECK(f.menu.fadeOpacity() == sample.labelAlpha);
                test::FakeRenderDevice device;
                const test::FakeTexture glow{64, 64};
                MenuTextures textures;
                textures.font = &f.sheet;
                textures.glow = &glow;
                Canvas canvas;
                canvas.begin(device, Mat4{1});
                f.menu.draw(canvas, f.painter, textures);
                canvas.end();
                bool sawGlow = false;
                bool sawText = false;
                for (const test::RecordedDraw& draw : device.draws) {
                    const bool isGlow = draw.texture == &glow;
                    const bool isText = draw.texture == &f.sheet;
                    sawGlow = sawGlow || isGlow;
                    sawText = sawText || isText;
                    if (isGlow || isText) {
                        for (const auto& vertex : draw.vertices) {
                            CHECK(vertex.color.a ==
                                  (isGlow ? sample.glowAlpha : sample.labelAlpha));
                        }
                    }
                }
                CHECK(sawGlow);
                CHECK(sawText);
            }
        }
    }
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

TEST_CASE("parchment menus share red labels and purple focus without unsolicited footers",
          "[game][menu]") {
    Fixture f;
    test::FakeRenderDevice device;
    const test::FakeTexture glow{64, 64};
    const test::FakeTexture parchment{64, 64};
    const test::FakeTexture backdrop{16, 16};
    MenuTextures textures;
    textures.font = &f.sheet;
    textures.glow = &glow;
    textures.parchment = &parchment;
    textures.backdrop = &backdrop;
    auto definition = MenuDefinition::parchment();
    definition.title = "A";
    definition.items = {{"B", 0}, {"C", 1}};
    CHECK_FALSE(definition.prompts);
    CHECK(definition.backLabel.empty());
    CHECK(definition.selectLabel.empty());
    CHECK(definition.colors.off == Color::rgba(92, 26, 3));
    CHECK(definition.colors.hi == Color::rgba(130, 0, 234));
    SECTION("the precolored parchment sheet supplies the title and inactive label") {
        f.menu.open(definition, f.painter, {});
        Canvas canvas;
        canvas.begin(device, Mat4{1});
        f.menu.draw(canvas, f.painter, textures);
        canvas.end();
        REQUIRE(device.draws.size() == 5);
        CHECK(device.draws[1].texture == &parchment); // title
        CHECK(device.draws[2].texture == &glow);
        CHECK(device.draws[2].vertices[0].color.r == definition.colors.hi.r);
        CHECK(device.draws[2].vertices[0].color.b == definition.colors.hi.b);
        CHECK(device.draws[4].texture == &parchment); // inactive item
    }
    SECTION("a missing parchment sheet falls back to red ink, never white labels") {
        textures.parchment = nullptr;
        f.menu.open(definition, f.painter, {});
        Canvas canvas;
        canvas.begin(device, Mat4{1});
        f.menu.draw(canvas, f.painter, textures);
        canvas.end();
        REQUIRE(device.draws.size() == 4); // The two plain-font items share a batch.
        CHECK(device.draws[1].texture == &f.sheet);
        CHECK(device.draws[1].vertices[0].color == definition.colors.off);
        CHECK(device.draws.back().vertices.back().color == definition.colors.off);
    }
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
    f32 promptLeft = 1000.0f;
    f32 promptRight = 0.0f;
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
    f32 top = 1000.0f;
    f32 bottom = 0.0f;
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
    test::convertModelFixture(dir);
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

TEST_CASE("a two-choice row highlights only its active part and renders one checkmark",
          "[game][menu][settings]") {
    auto font = BitmapFont::fromGlyphs(10, 4, {{'A', 8, 0, 0}, {'B', 8, 8, 0}, {'~', 8, 16, 0}});
    test::FakeTexture sheet{32, 32};
    test::FakeTexture glow{32, 32};
    TextPainter painter;
    painter.setFont(&font, &sheet);
    MenuTextures textures;
    textures.font = &sheet;
    textures.glow = &glow;
    MenuDefinition definition;
    definition.x = 100;
    definition.y = 100;
    definition.items = {{"A", 0, 0, true, "B", 2}};
    OptionMenu menu;
    menu.open(definition, painter, {});
    const auto verify = [&](s32 selectedPart) {
        menu.markItem(0, selectedPart);
        test::FakeRenderDevice device;
        Canvas canvas;
        canvas.begin(device, Mat4{1});
        menu.draw(canvas, painter, textures);
        canvas.end();
        s32 marks = 0;
        s32 glowingGlyphs = 0;
        for (const auto& draw : device.draws) {
            for (usize i = 0; i < draw.vertices.size(); i += 6) {
                if (draw.texture == &glow) {
                    ++glowingGlyphs;
                    CHECK(draw.vertices[i].position.x ==
                          100 + TextPainter::kCellInset +
                              (selectedPart == 2 ? painter.measure("A    ", 1) : 0) -
                              OptionMenu::kGlowExpand);
                } else if (draw.vertices[i].uv.x == (16 + TextPainter::kCellInset) / 32) {
                    ++marks;
                }
            }
        }
        CHECK(glowingGlyphs == 1);
        CHECK(marks == 1);
    };
    verify(2);
    verify(1);
}

TEST_CASE("rows and a title wider than the width a menu is given shrink to fit inside it",
          "[game][menu][fit]") {
    Fixture f;
    // Every letter is eight pixels wide: a forty-letter row is 320 wide at scale one, wider
    // than the dialog's parchment less its margins.
    auto dialog = MenuDefinition::confirmation("A TITLE THAT IS FAR TOO LONG FOR THE SCROLL",
                                               "RETURN TO THE MAIN MENU OF THE WHOLE GAME",
                                               "QUIT TO THE DESKTOP");
    CHECK(dialog.fitWidth == 320);
    f.menu.open(dialog, f.painter, MenuScreen{});
    const auto& fitted = f.menu.definition();
    CHECK(fitted.scale < 1.0f);
    CHECK(fitted.cursorScale == fitted.scale);
    CHECK(fitted.titleScale < 1.2f);
    const s32 room = 320 - 2 * OptionMenu::kFitMargin;
    CHECK(f.painter.measure(fitted.items[0].text, fitted.scale) <= room);
    CHECK(f.painter.measure(fitted.title, fitted.titleScale) <= room);
    for (usize i = 0; i < fitted.items.size(); ++i) {
        const Rect area = f.menu.itemArea(i);
        CHECK(area.x >= f.menu.backdropArea().x + OptionMenu::kFitMargin);
        CHECK(area.x + area.width <= f.menu.backdropArea().x + 320 - OptionMenu::kFitMargin);
    }
    // Rows that fit are left at their size.
    f.menu.open(MenuDefinition::confirmation("QUIT?", "NO", "YES"), f.painter, MenuScreen{});
    CHECK(f.menu.definition().scale == 1.0f);
    CHECK(f.menu.definition().titleScale == 1.2f);
    // Without a width to fit, nothing changes however long the row.
    MenuDefinition free;
    free.items = {{"A ROW AS LONG AS THE WHOLE SCREEN AND MORE BESIDES IT", 0}};
    f.menu.open(free, f.painter, MenuScreen{});
    CHECK(f.menu.definition().scale == 1.0f);
}

TEST_CASE("the bar at the column's right hand is clicked and dragged through the column",
          "[game][menu][scroll-menu][mouse]") {
    Fixture f;
    MenuDefinition definition;
    for (s32 i = 0; i < 8; ++i) {
        definition.items.push_back({std::string(1, static_cast<char>('A' + i)), i});
    }
    definition.visibleRows = 3;
    definition.x = 10;
    definition.y = 20;
    f.menu.open(definition, f.painter, MenuScreen{});
    const Rect bar = f.menu.scrollBarArea();
    REQUIRE(bar.width > 0);
    CHECK(bar.x > static_cast<f32>(f.menu.columnRight()));
    // Pressed on the track, the thumb follows the pointer while it is held.
    MenuInput press;
    press.pointer = Vec2{bar.x + bar.width / 2, bar.y + bar.height / 2};
    press.pointerPressed = true;
    press.pointerHeld = true;
    f.menu.update(press, 1);
    MenuInput drag;
    drag.pointer = Vec2{bar.x + bar.width / 2, bar.y + bar.height}; // the bottom
    drag.pointerHeld = true;
    f.menu.update(drag, 1);
    CHECK(f.menu.scroll() == 5);
    CHECK(f.menu.selection() == 0); // the selection stays where it was
    // The icon stays with the rows in view, at the edge nearest its selection.
    f.menu.update(drag, OptionMenu::kIconGlideTicks);
    f.menu.update(drag, OptionMenu::kIconGlideTicks);
    CHECK(f.menu.iconY() == f.menu.itemY(5) + f.menu.lineHeight() / 2);
    CHECK(f.menu.iconY() >= f.menu.itemY(5));
    CHECK(f.menu.iconY() <= f.menu.itemY(7) + f.menu.lineHeight());
    drag.pointer = Vec2{bar.x + bar.width / 2, bar.y}; // the top
    f.menu.update(drag, 1);
    CHECK(f.menu.scroll() == 0);
    f.menu.update(drag, OptionMenu::kIconGlideTicks);
    f.menu.update(drag, OptionMenu::kIconGlideTicks);
    CHECK(f.menu.iconY() == f.menu.itemY(0) + f.menu.lineHeight() / 2); // back on its row
    // Let go, the pointer no longer moves the window.
    MenuInput release;
    release.pointer = Vec2{bar.x + bar.width / 2, bar.y + bar.height};
    f.menu.update(release, 1);
    f.menu.update(release, 1);
    CHECK(f.menu.scroll() == 0);
    // The marks above and below step a row.
    f.menu.scrollWindow(2);
    MenuInput mark;
    mark.pointer = Vec2{bar.x + bar.width / 2, bar.y + 1};
    mark.pointerPressed = true;
    f.menu.update(mark, 1);
    CHECK(f.menu.scroll() == 1);
    mark.pointer = Vec2{bar.x + bar.width / 2, bar.y + bar.height - 1};
    f.menu.update(mark, 1);
    f.menu.update({}, 1);
    f.menu.update(mark, 1);
    CHECK(f.menu.scroll() == 3);
    // A pointer that comes to rest over a row chooses it; resting still, it chooses nothing
    // more as the rows scroll under it, until it moves again.
    MenuInput rest;
    rest.pointer = Vec2{f.menu.itemArea(3).x + 1, f.menu.itemArea(3).y + 1};
    f.menu.update(rest, 1);
    CHECK(f.menu.selection() == 3);
    f.menu.scrollWindow(5); // row 5 now stands where row 3 stood, under the pointer
    f.menu.update(rest, 1);
    CHECK(f.menu.selection() == 3);
    rest.pointer = Vec2{rest.pointer->x + OptionMenu::kHoverMotion + 1, rest.pointer->y};
    f.menu.update(rest, 1);
    CHECK(f.menu.selection() == 5);
}

TEST_CASE("a dialog's rows stand evenly on its parchment, under its title, with a gap between",
          "[game][menu][fit]") {
    Fixture f;
    // Without a title the rows sit in the middle of the parchment (64 to 284), ten apart.
    auto dialog = MenuDefinition::confirmation({}, "MAIN", "QUIT");
    CHECK(dialog.rowGap == OptionMenu::kDialogRowGap);
    CHECK(dialog.centreOnBackdrop);
    f.menu.open(dialog, f.painter, MenuScreen{});
    CHECK(f.menu.titleBottom() == 0);
    CHECK(f.menu.columnHeight() == 2 * f.menu.lineHeight() + OptionMenu::kDialogRowGap);
    CHECK(f.menu.itemY(1) == f.menu.itemY(0) + f.menu.lineHeight() + OptionMenu::kDialogRowGap);
    const s32 middle = 64 + 220 / 2;
    CHECK(f.menu.itemY(0) == middle - f.menu.columnHeight() / 2);
    // With a title they sit in the middle of what lies under it.
    f.menu.open(MenuDefinition::confirmation("SURE?", "NO", "YES"), f.painter, MenuScreen{});
    const s32 titleBottom = 64 + OptionMenu::kTitleMargin + f.painter.lineHeight(1.2f);
    CHECK(f.menu.titleBottom() == titleBottom);
    CHECK(f.menu.itemY(0) == (titleBottom + 284) / 2 - f.menu.columnHeight() / 2);
    CHECK(f.menu.itemY(0) >= titleBottom);
    // Rows that would stand over a page's title move down under it: a column as one, rows
    // placed by hand together.
    auto page = MenuDefinition::parchment();
    page.title = "T";
    page.items = {{"A", 0}, {"B", 1}};
    page.y = 70;
    f.menu.open(page, f.painter, MenuScreen{});
    const s32 pageTitleBottom = 8 + OptionMenu::kTitleMargin + f.painter.lineHeight(1.2f);
    CHECK(f.menu.itemY(0) == pageTitleBottom);
    page.itemPositions = {Vec2{64, 70}, Vec2{64, 90}};
    f.menu.open(page, f.painter, MenuScreen{});
    CHECK(f.menu.itemY(0) == pageTitleBottom);
    CHECK(f.menu.itemY(1) == pageTitleBottom + 20);
    // A column already under the title is left where it stands.
    page.itemPositions.clear();
    page.y = 200;
    f.menu.open(page, f.painter, MenuScreen{});
    CHECK(f.menu.itemY(0) == 200);
}

} // namespace
