#include <array>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "RecordedQuads.h"
#include "TestSupport.h"
#include "game/characters/ClassData.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/hud/StatusBox.h"

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
    painter.draw(canvas, 0, view, StatusBoxPainter::Strip::Play);
    canvas.end();
    const usize full = device.draws.size();
    REQUIRE(full >= 4); // the bar, the panel, the frame, the icons and the text
    REQUIRE(device.draws[1].vertices.front().color == boxTint(view.color, true));
    TextureSet art;
    REQUIRE(art.load(root / "STATIC"));
    const auto horns = art.find("BK_RUNE_STONE_02");
    REQUIRE(horns);
    const auto& hornTexture = dynamic_cast<const test::FakeTexture&>(art.texture(device, *horns));
    const auto* drawnTexture = dynamic_cast<const test::FakeTexture*>(device.draws[0].texture);
    REQUIRE(drawnTexture != nullptr);
    CHECK(drawnTexture->pixels == hornTexture.pixels);
    // setup_player_display / mbBlitProject retain the native 128x16 strip at y304.
    CHECK(hornTexture.width() == 128);
    CHECK(hornTexture.height() == 16);
    CHECK(test::minCorner(device.draws[0]) == Vec2{0, 304});
    CHECK(test::maxCorner(device.draws[0]) == Vec2{128, 320});
    CHECK(device.draws[0].vertices.front().color == Color::white());

    // Keys and potions carried add their icons and counts over the gold and the health.
    view.keys = 3;
    view.potions = 2;
    view.potionKind = 4;
    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    painter.draw(canvas, 0, view, StatusBoxPainter::Strip::Play);
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

    // Fallen out of the tower: choices remain, but do not resolve button hints.
    StatusBoxView fallen = view;
    fallen.inTower = true;
    fallen.towerPrompt = true;
    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    painter.draw(canvas, 1, fallen, StatusBoxPainter::Strip::Play);
    canvas.end();
    for (const test::RecordedDraw& draw : device.draws) {
        const Vec2 corner = test::minCorner(draw);
        CHECK(corner != Vec2{128.0f + 8.0f, 323.0f}); // no key icon
    }
    // Text batches by font, so a line's absence shows in the vertices, not the draws.
    const auto vertices = [&device] {
        usize count = 0;
        for (const test::RecordedDraw& draw : device.draws) {
            count += draw.vertices.size();
        }
        return count;
    };
    const usize offered = vertices();
    fallen.quitOffered = false; // an online match's prompt offers no Quit Game
    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    painter.draw(canvas, 1, fallen, StatusBoxPainter::Strip::Play);
    canvas.end();
    CHECK(vertices() < offered);
    CHECK(vertices() > 0);

    // A pickup card at the bar over the box, and a count above it.
    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    painter.drawCard(canvas, 0, "CRYSTAL", 304);
    const std::array<StatusBoxPainter::CountEntry, 1> entries{{{"SM_CRYSTAL_ORA", 3, 15}}};
    painter.drawCounts(canvas, 0, entries, 288);
    canvas.end();
    REQUIRE(device.draws.size() >= 4); // the strip, the card, the icon, the digits
    REQUIRE(test::minCorner(device.draws[0]) == Vec2{0.0f, 304.0f});
    REQUIRE(test::minCorner(device.draws[1]) == Vec2{0.0f, 320.0f});
    REQUIRE(test::maxCorner(device.draws[1]) == Vec2{128.0f, 384.0f});
    REQUIRE(test::minCorner(device.draws[2]).y == 288.0f);
    REQUIRE(test::minCorner(device.draws[2]).x > 0.0f);
    REQUIRE(test::minCorner(device.draws[3]).x >= test::minCorner(device.draws[2]).x + 20.0f);

    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    painter.draw(canvas, 1, StatusBoxView{}, StatusBoxPainter::Strip::None);
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
    // The thirteenth has its own icon, sixteen square, eight in at 340 (hod_blit).
    CHECK(corners(StatusBoxPainter::kThirteenthRune, std::nullopt) ==
          std::vector<Vec2>{{128.0f + 8.0f, 340.0f}});
    device.draws.clear();
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    painter.drawRelics(canvas, 1, StatusBoxPainter::kThirteenthRune, std::nullopt);
    canvas.end();
    REQUIRE(device.draws.size() == 1);
    CHECK(test::maxCorner(device.draws[0]) == Vec2{128.0f + 24.0f, 356.0f});

    // The eight keys take the GameCube's key table, first to eighth (0x801205F8, player.c 5142),
    // whose fifth to eighth are green, red, yellow and blue. The canvas batches the fourth and
    // fifth, both green, into one draw, so the keys are read quad by quad.
    constexpr std::array<std::string_view, static_cast<usize>(StatusBoxPainter::kKeyCount)>
        kGameCubeKeys{"BLU", "RED", "YEL", "GRE", "GRE", "RED", "YEL", "BLU"};
    constexpr u16 kEveryKey = 0xFF;
    const auto drawKeys = [&](StatusBoxPainter& boxes) {
        device.draws.clear();
        canvas.begin(device, Mat4{1.0f});
        boxes.drawRelics(canvas, 1, 0, kEveryKey);
        canvas.end();
        return test::quadsOf(device);
    };
    const std::vector<test::Quad> drawn = drawKeys(painter);
    REQUIRE(drawn.size() == kGameCubeKeys.size());
    for (usize key = 0; key < kGameCubeKeys.size(); ++key) {
        CAPTURE(key);
        const Texture* picture =
            painter.staticTexture(std::format("SM_KEY_{}", kGameCubeKeys[key]));
        REQUIRE(picture != nullptr);
        CHECK(drawn[key].texture == picture);
        CHECK(drawn[key].area == Rect{128.0f + 12.0f * static_cast<f32>(key + 1), 300.0f,
                                      static_cast<f32>(picture->width()),
                                      static_cast<f32>(picture->height())});
    }
    painter.release();

    // Warming the boxes takes every picture the strip draws, each key's and each rune's, the
    // thirteenth's too, so showing them all uploads nothing more.
    StatusBoxPainter warmed;
    REQUIRE(warmed.load(device, root, nullptr));
    warmed.preloadStatus();
    const u32 preloaded = device.texturesCreated;
    CHECK(drawKeys(warmed).size() == kGameCubeKeys.size());
    constexpr auto kEveryRune = static_cast<u16>(((1U << StatusBoxPainter::kRuneCount) - 1U) |
                                                 StatusBoxPainter::kThirteenthRune);
    device.draws.clear();
    canvas.begin(device, Mat4{1.0f});
    warmed.drawRelics(canvas, 1, kEveryRune, std::nullopt);
    canvas.end();
    CHECK(test::quadsOf(device).size() == StatusBoxPainter::kRuneCount + 1U);
    CHECK(device.texturesCreated == preloaded);
    warmed.release();
}

TEST_CASE("the boxes lend the textures they draw with", "[game][screens][relic-strip][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::FakeRenderDevice device;
    StatusBoxPainter painter;
    REQUIRE(painter.load(device, root, nullptr));
    // A level's own pickup icon, which STATIC does not have.
    const std::string levelIcon = "LEVEL_PICKUP_ICON";
    auto level =
        TextureSet::fromDecoded({{.name = levelIcon, .width = 16, .height = 16, .file = {}}},
                                {Image::filled(16, 16, Color::white())});
    painter.setCountTextures(&level);

    // The warrior's box with the first boss's key up, a card over the second box, and two
    // counts over the third: STATIC's crystal, and the level's own icon above it.
    StatusBoxView view;
    view.mode = StatusBoxView::Mode::Status;
    view.active = true;
    view.classIndex = 0;
    view.bossKeys = 0x1;
    view.keysShown = true;
    const std::array<StatusBoxPainter::CountEntry, 2> counts{
        {{"SM_CRYSTAL_ORA", 3, 15}, {levelIcon, 1, 12}}};
    Canvas canvas;
    const auto draw = [&] {
        device.draws.clear();
        canvas.begin(device, Mat4{1.0f});
        painter.draw(canvas, 0, view, StatusBoxPainter::Strip::Play);
        painter.drawCard(canvas, 1, "CRYSTAL", 304);
        painter.drawCounts(canvas, 2, counts, 288);
        canvas.end();
    };
    // Where the one draw of `texture` lies; none unless exactly one draw used it.
    const auto areaOf = [&device](const Texture* texture) -> std::optional<Rect> {
        std::optional<Rect> area;
        usize found = 0;
        for (const test::RecordedDraw& recorded : device.draws) {
            if (recorded.texture == texture) {
                const Vec2 low = test::minCorner(recorded);
                const Vec2 high = test::maxCorner(recorded);
                area = Rect{low.x, low.y, high.x - low.x, high.y - low.y};
                ++found;
            }
        }
        return found == 1 ? area : std::nullopt;
    };
    draw();
    const u32 created = device.texturesCreated;

    // Each lender gives the very texture drawn, and uploads nothing to do so.
    const Texture* panel = painter.selectTexture("S4_WAR");
    const Texture* key = painter.staticTexture("SM_KEY_BLU");
    const Texture* strip = painter.staticTexture(StatusBoxPainter::kStrip);
    const Texture* crystal = painter.countTexture("SM_CRYSTAL_ORA");
    const Texture* own = painter.countTexture(levelIcon);
    REQUIRE(panel != nullptr);
    REQUIRE(key != nullptr);
    REQUIRE(strip != nullptr);
    REQUIRE(crystal != nullptr);
    REQUIRE(own != nullptr);
    CHECK(device.texturesCreated == created);
    CHECK(own == &level.texture(device, 0));
    CHECK(areaOf(panel) == Rect{0.0f, 320.0f, 128.0f, 64.0f});
    CHECK(areaOf(key) ==
          Rect{12.0f, 300.0f, static_cast<f32>(key->width()), static_cast<f32>(key->height())});
    CHECK(areaOf(strip) == Rect{128.0f, 304.0f, 128.0f, 16.0f});
    const auto icon = static_cast<f32>(StatusBoxPainter::kCountIconSize);
    const std::optional<Rect> crystalArea = areaOf(crystal);
    REQUIRE(crystalArea.has_value());
    CHECK(crystalArea->x > 256.0f);
    CHECK(crystalArea->y == 288.0f);
    CHECK(crystalArea->width == icon);
    CHECK(crystalArea->height == icon);
    const std::optional<Rect> ownArea = areaOf(own);
    REQUIRE(ownArea.has_value());
    CHECK(ownArea->x > 256.0f);
    CHECK(ownArea->y == 288.0f - icon - static_cast<f32>(StatusBoxPainter::kCountRowGap));
    CHECK(ownArea->width == icon);
    CHECK(ownArea->height == icon);
    CHECK(painter.selectTexture("S4_WAR") == panel);
    CHECK(painter.staticTexture("SM_KEY_BLU") == key);
    CHECK(painter.countTexture("SM_CRYSTAL_ORA") == crystal);
    CHECK(painter.countTexture(levelIcon) == own);
    CHECK(device.texturesCreated == created);

    // A picture lent before the boxes draw it is uploaded once, and drawn as lent.
    const Texture* second = painter.staticTexture("SM_KEY_RED");
    REQUIRE(second != nullptr);
    CHECK(device.texturesCreated == created + 1);
    CHECK(painter.staticTexture("SM_KEY_RED") == second);
    view.bossKeys = 0x2;
    draw();
    CHECK(device.texturesCreated == created + 1);
    CHECK(areaOf(second) == Rect{24.0f, 300.0f, static_cast<f32>(second->width()),
                                 static_cast<f32>(second->height())});

    // What the sets do not hold, or a released painter, lends nothing.
    CHECK(painter.staticTexture("NO_SUCH_PICTURE") == nullptr);
    CHECK(painter.selectTexture("NO_SUCH_PICTURE") == nullptr);
    CHECK(painter.countTexture("NO_SUCH_PICTURE") == nullptr);
    CHECK(device.texturesCreated == created + 1);
    painter.release();
    CHECK(painter.staticTexture("SM_KEY_BLU") == nullptr);
    CHECK(painter.selectTexture("S4_WAR") == nullptr);
    CHECK(painter.countTexture(levelIcon) == nullptr);
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
    slot = PowerupSlot{-1, powerup::kWeapon, 12, damage::kSuperShot, true};
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
        painter.draw(canvas, player, view, StatusBoxPainter::Strip::Play);
        canvas.end();
        const auto baseline = device.draws;
        const Texture* score = nullptr;
        for (const auto& draw : baseline) {
            if (test::minCorner(draw).y == 359.5f) {
                score = draw.texture;
            }
        }
        REQUIRE(score != nullptr);
        view.powerup = PowerupSlot{9999.96f, powerup::kArmor, 0, armor::kInvulnerable, true};
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        painter.draw(canvas, player, view, StatusBoxPainter::Strip::Play);
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
