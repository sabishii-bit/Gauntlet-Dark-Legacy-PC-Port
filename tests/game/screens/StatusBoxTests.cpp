#include <filesystem>
#include <limits>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/StatusBox.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("status boxes draw a player's panel and a dimmed empty slot", "[game][screens][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
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

    // Fallen out of the tower: mapped confirm/cancel beside waiting and quitting,
    // in place of the gold and health (player.c 1397).
    StatusBoxView fallen = view;
    fallen.inTower = true;
    fallen.towerPrompt = true;
    std::vector<std::string> promptedActions;
    painter.setControlLabels([&](s32 player, std::string_view action) {
        CHECK(player == 1);
        promptedActions.emplace_back(action);
        return "F2";
    });
    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    painter.draw(canvas, 1, fallen, true);
    canvas.end();
    bool wait = false;
    bool quit = false;
    for (const test::RecordedDraw& draw : device.draws) {
        const Vec2 corner = test::minCorner(draw);
        for (const auto& vertex : draw.vertices) {
            const auto& position = vertex.position;
            wait = wait ||
                   (position.x >= 134 && position.x < 154 && position.y >= 332 && position.y < 352);
            quit = quit ||
                   (position.x >= 134 && position.x < 154 && position.y >= 352 && position.y < 372);
        }
        CHECK(corner != Vec2{128.0f + 8.0f, 323.0f}); // no key icon
    }
    CHECK(wait);
    CHECK(quit);
    CHECK(promptedActions == std::vector<std::string>{"menuSelect", "menuBack"});

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

TEST_CASE("the runestones held line the box, the bosses' keys a while as a level opens",
          "[game][screens][relic-strip][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    StatusBoxPainter painter;
    REQUIRE(painter.load(device, root, nullptr));
    const auto corners = [&](u16 runes, std::optional<u16> keys) {
        device.draws.clear();
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        painter.drawRelics(canvas, 1, runes, keys);
        canvas.end();
        std::vector<Vec2> found;
        found.reserve(device.draws.size());
        for (const test::RecordedDraw& draw : device.draws) {
            found.push_back(test::minCorner(draw));
        }
        return found;
    };
    // Runes 1, 5 and 12 of the second box: 15 in, 8 a rune and a pixel a colour (player.c 5133).
    const std::vector<Vec2> runes = corners(0x0811, std::nullopt);
    CHECK(runes == std::vector<Vec2>{{128.0f + 15.0f, 306.0f},
                                     {128.0f + 15.0f + 32.0f + 1.0f, 306.0f},
                                     {128.0f + 15.0f + 88.0f + 3.0f, 306.0f}});
    // The second key, 12 in and 12 a key, at 300; none when they are not shown.
    const std::vector<Vec2> keys = corners(0, u16{0x2});
    CHECK(keys == std::vector<Vec2>{{128.0f + 24.0f, 300.0f}});
    CHECK(corners(0, std::nullopt).empty());
    painter.release();
}

TEST_CASE("status boxes need the unpacked panels", "[game][screens]") {
    test::FakeRenderDevice device;
    StatusBoxPainter painter;
    REQUIRE_FALSE(painter.load(device, test::scratchDirectory("status-box-none"), nullptr));
    REQUIRE_FALSE(painter.loaded());
}

TEST_CASE("powerup usage rounds time to one decimal and shows whole remaining charges",
          "[game][screens][powerup-usage]") {
    PowerupSlot slot{12.36f, powerup::kSpeed, 50, 0, true};
    CHECK(StatusBoxPainter::usageAmount(slot) == "12.4");
    slot.strength = 9.96f;
    CHECK(StatusBoxPainter::usageAmount(slot) == "10.0");
    slot.strength = 0.04f;
    CHECK(StatusBoxPainter::usageAmount(slot) == "0.0");
    slot.strength = -1;
    CHECK(StatusBoxPainter::usageAmount(slot).empty());
    slot = PowerupSlot{-1, powerup::kWeapon, 12, powerup::kSuperShot, true};
    CHECK(StatusBoxPainter::usageAmount(slot) == "12");
    slot.on = false;
    CHECK(StatusBoxPainter::usageAmount(slot).empty());
    slot.on = true;
    slot.strength = 0;
    CHECK(StatusBoxPainter::usageAmount(slot).empty());
    slot.strength = -1;
    slot.charge = -1;
    CHECK(StatusBoxPainter::usageAmount(slot).empty());
    slot.charge = std::numeric_limits<f32>::infinity();
    CHECK(StatusBoxPainter::usageAmount(slot).empty());
}

TEST_CASE("powerup usage fits the unused bottom of all four full status cards in the score font",
          "[game][screens][powerup-usage][assets]") {
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    test::FakeRenderDevice device;
    StatusBoxPainter painter;
    REQUIRE(painter.load(device, root, &strings));
    Canvas canvas;
    StatusBoxView view;
    view.mode = StatusBoxView::Mode::Status;
    view.active = true;
    view.name = "WWWWWW";
    view.gold = 99999;
    view.health = 10400;
    view.keys = 9;
    view.potions = 9;
    view.level = 99;
    view.runes = 0xFFF;
    view.bossKeys = 0xFF;
    view.keysShown = true;
    for (s32 player = 0; player < 4; ++player) {
        CAPTURE(player);
        view.powerup.reset();
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        painter.draw(canvas, player, view, true);
        canvas.end();
        const auto baseline = device.draws;
        const Texture* score = nullptr;
        for (const auto& draw : baseline) {
            if (test::minCorner(draw).y == 359.5f) {
                score = draw.texture;
            }
        }
        REQUIRE(score != nullptr);
        view.powerup = PowerupSlot{9999.96f, powerup::kArmor, 0, powerup::kInvulnerable, true};
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        painter.draw(canvas, player, view, true);
        canvas.end();
        REQUIRE(device.draws.size() > baseline.size());
        for (usize i = 0; i < baseline.size(); ++i) {
            CHECK(test::minCorner(device.draws[i]) == test::minCorner(baseline[i]));
            CHECK(test::maxCorner(device.draws[i]) == test::maxCorner(baseline[i]));
        }
        bool scoreDigits = false;
        bool decimal = false;
        for (usize i = baseline.size(); i < device.draws.size(); ++i) {
            const auto& draw = device.draws[i];
            const Vec2 low = test::minCorner(draw);
            const Vec2 high = test::maxCorner(draw);
            CHECK(low.x >= static_cast<f32>(player * StatusBoxPainter::kWidth +
                                            StatusBoxPainter::kUsageLeft));
            CHECK(high.x <= static_cast<f32>(player * StatusBoxPainter::kWidth +
                                             StatusBoxPainter::kUsageRight));
            CHECK(low.y >= static_cast<f32>(StatusBoxPainter::kUsageY));
            CHECK(high.y < static_cast<f32>(StatusBoxPainter::kY + StatusBoxPainter::kHeight));
            scoreDigits = scoreDigits || draw.texture == score;
            decimal = decimal || draw.texture == &device.whiteTexture();
        }
        CHECK(scoreDigits);
        CHECK(decimal);
    }
}

} // namespace
