#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/StatusBox.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("status boxes draw a player's panel and a dimmed empty slot", "[game][screens][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("STATIC/textures.json").parent_path().parent_path();
    test::FakeRenderDevice device;
    StatusBoxPainter painter;
    REQUIRE_FALSE(painter.loaded());
    REQUIRE(painter.load(device, root, nullptr));
    REQUIRE(painter.loaded());

    StatusBoxView view;
    view.mode = StatusBoxView::Mode::Status;
    view.active = true;
    view.classIndex = 1;
    view.color = 2;
    view.name = "VAL";
    view.level = 3;
    view.gold = 250;
    view.health = 480;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    painter.draw(canvas, 0, view, true);
    canvas.end();
    const usize full = device.draws.size();
    REQUIRE(full >= 4); // the bar, the panel, the frame, the icons and the text

    // Keys and potions carried add their icons and counts over the gold and the health.
    view.keys = 3;
    view.potions = 2;
    view.potionKind = 4;
    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    painter.draw(canvas, 0, view, true);
    canvas.end();
    REQUIRE(device.draws.size() > full);
    bool keyIcon = false;
    bool potionIcon = false;
    for (const test::RecordedDraw& draw : device.draws) {
        const Vec2 corner = test::minCorner(draw);
        keyIcon = keyIcon || corner == Vec2{8.0f, 323.0f};
        potionIcon = potionIcon || corner == Vec2{102.0f, 323.0f};
    }
    REQUIRE(keyIcon);
    REQUIRE(potionIcon);
    REQUIRE(StatusBoxPainter::potionIcon(2) == "POTION_ICON_BLU");
    REQUIRE(StatusBoxPainter::potionIcon(4) == "POTION_ICON_GRE");
    REQUIRE(StatusBoxPainter::potionIcon(77) == "POTION_ICON_RED");

    // A pickup card at the bar over the box, and a count above it.
    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    painter.drawCard(canvas, 0, "CRYSTAL", 304);
    painter.drawCount(canvas, 0, "SM_CRYSTAL_ORA", 3, 15);
    canvas.end();
    REQUIRE(device.draws.size() >= 4); // the strip, the card, the icon, the digits
    REQUIRE(test::minCorner(device.draws[0]) == Vec2{0.0f, 304.0f});
    REQUIRE(test::minCorner(device.draws[1]) == Vec2{0.0f, 320.0f});
    REQUIRE(test::maxCorner(device.draws[1]) == Vec2{128.0f, 384.0f});
    REQUIRE(test::minCorner(device.draws[2]) == Vec2{28.0f, 288.0f});
    REQUIRE(test::minCorner(device.draws[3]).x >= 48.0f);

    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    painter.draw(canvas, 1, StatusBoxView{}, false);
    canvas.end();
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE(device.draws.size() < full);
    REQUIRE(test::minCorner(device.draws[0]).x >= static_cast<f32>(StatusBoxPainter::kWidth));

    painter.release();
    REQUIRE_FALSE(painter.loaded());
}

TEST_CASE("status boxes need the unpacked panels", "[game][screens]") {
    test::FakeRenderDevice device;
    StatusBoxPainter painter;
    REQUIRE_FALSE(painter.load(device, test::scratchDirectory("status-box-none"), nullptr));
    REQUIRE_FALSE(painter.loaded());
}

} // namespace
