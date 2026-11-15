#include <array>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "RecordedDrawChecks.h"
#include "TestSupport.h"
#include "game/audio/LevelSoundscape.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Inventory.h"
#include "game/combat/PowerupFlags.h"
#include "game/config/GameConfig.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/DarkLegacyHudPainter.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudView.h"
#include "game/hud/PartyHud.h"
#include "game/hud/PickupHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/StatusBox.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerControls.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** Writes a set of 2 by 2 pictures named `names` into `directory`. */
void writePictures(const std::filesystem::path& directory,
                   std::initializer_list<std::string_view> names) {
    std::filesystem::create_directories(directory);
    writeFile(directory / "tiny.png", test::kTinyPng);
    std::string bitmaps;
    for (const std::string_view name : names) {
        bitmaps += std::format(R"({}{{"name": "{}", "width": 2, "height": 2, "file": "tiny.png"}})",
                               bitmaps.empty() ? "" : ", ", name);
    }
    writeTextFile(directory / "textures.json", std::format(R"({{"bitmaps": [{}]}})", bitmaps));
}

/** The least the boxes load from: SELECT, STATIC with the initials' sheet, and that font. */
std::filesystem::path plainRoot(std::string_view name) {
    const auto root = test::scratchDirectory(name);
    writePictures(root / "SELECT", {"S4_WAR"});
    writePictures(root / "STATIC", {"INITIALS"});
    std::filesystem::create_directories(root / "fonts");
    writeTextFile(root / "fonts/initials.json",
                  R"({"height": 2, "glyphs": [{"code": 65, "width": 2, "x": 0, "y": 0}]})");
    return root;
}

/** Every seat of `players` as the scene hands it to the look, under `hud`'s selectors and with
 * its cards and counts. */
HudView viewOf(std::span<const PlayerRuntime> players, const PartyHud& hud,
               const HudSeatRules& rules) {
    const ClassDataSet classes;
    HudView view;
    view.players = players;
    view.pickups = &hud.pickups();
    for (usize seat = 0; seat < view.seats.size(); ++seat) {
        const auto index = static_cast<s32>(seat);
        view.seats[seat] = hudSeatOf(index, players, hud.selector(index), classes, rules);
    }
    return view;
}

/** The index of the first or last draw of `texture`; none when nothing drew it. */
std::optional<usize> drawOf(const test::FakeRenderDevice& device, const Texture* texture,
                            bool last) {
    std::optional<usize> found;
    for (usize index = 0; index < device.draws.size(); ++index) {
        if (device.draws[index].texture == texture) {
            found = index;
            if (!last) {
                break;
            }
        }
    }
    return found;
}

TEST_CASE("the Dark Legacy look keeps the companion arrows above its boxes", "[dark-legacy-hud]") {
    DarkLegacyHudPainter look;
    CHECK(look.statusTop() == 304.0f);
    CHECK(look.statusTop() == static_cast<f32>(StatusBoxPainter::kBarY));
    CHECK(look.id() == kDarkLegacyHudStyle);
    // The look it lends is itself.
    DarkLegacyHudArt& art = look;
    CHECK(&art.look() == static_cast<HudPainter*>(&look));
}

TEST_CASE("the Dark Legacy look draws four boxes, then the cards, then the tickers",
          "[dark-legacy-hud][assets]") {
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    test::FakeRenderDevice device;
    DarkLegacyHudPainter look;
    REQUIRE(look.load(device, root, &strings));

    // Four players, seat 2 naming a powerup over its box; a card rising over seat 1 and a
    // count over seat 2, which stacks above the label.
    std::array<PlayerRuntime, 4> players;
    for (usize index = 0; index < players.size(); ++index) {
        CharacterSave save;
        save.name = std::format("P{}", index);
        save.character = static_cast<s32>(index);
        save.color = static_cast<s32>(index);
        save.progress().health = 400;
        players.at(index).actor.spawn(static_cast<s32>(index), save, nullptr, Vec3{0}, 0);
    }
    players[2].actor.save().progress().inventory.powerups[0] =
        PowerupSlot{30, powerup::kSpecial, 0, powerup::kInvisible, false};
    PartyHud hud;
    LevelSoundscape audio;
    hud.stepSelector(players[2].actor, SelectorInput{.up = true},
                     PowerupSelector::kSlide / PowerupSelector::kSlidePerTick, audio);
    hud.stepSelector(players[2].actor, {}, 1, audio);
    REQUIRE(hud.selector(2).showing());
    hud.pickups().addCard(1, PickupHud::kCrystalCard);
    hud.pickups().step(20, 0);
    hud.pickups().showCount(2, PickupHud::crystalIcon(1), 3, 15);
    REQUIRE(hud.pickups().cards().size() == 1);
    const s32 cardY = hud.pickups().cards().front().y;
    const s32 countY = PickupHud::countRowY(hud.selector(2).labelY(StatusBoxPainter::kY));
    REQUIRE(countY != PickupHud::countRowY(std::nullopt));

    const HudView view = viewOf(players, hud, {});
    Canvas canvas;
    HudFrame frame{.canvas = canvas, .device = device, .darkLegacy = look};
    canvas.begin(device, Mat4{1});
    look.drawStatus(frame, view);
    canvas.end();

    // The same parts drawn one by one with the boxes' own painter, in the look's order.
    test::FakeRenderDevice expectedDevice;
    StatusBoxPainter boxes;
    REQUIRE(boxes.load(expectedDevice, root, &strings));
    canvas.begin(expectedDevice, Mat4{1});
    for (usize seat = 0; seat < view.seats.size(); ++seat) {
        boxes.draw(canvas, static_cast<s32>(seat), view.seats[seat].box,
                   StatusBoxPainter::Strip::Play);
    }
    boxes.drawCard(canvas, 1, PickupHud::kCrystalCard, cardY);
    const std::array<StatusBoxPainter::CountEntry, 1> count{
        {{.icon = PickupHud::crystalIcon(1), .count = 3, .total = 15}}};
    boxes.drawCounts(canvas, 2, count, countY);
    canvas.end();
    REQUIRE(device.draws.size() == expectedDevice.draws.size());
    for (usize index = 0; index < device.draws.size(); ++index) {
        CAPTURE(index);
        test::checkRecordedDraw(device.draws[index], expectedDevice.draws[index]);
    }

    // The card over seat 1 and the count over seat 2's label, both after the last box.
    const auto lastFrame = drawOf(device, look.staticArt("S4_FRAME"), true);
    const auto card = drawOf(device, look.staticArt(PickupHud::kCrystalCard), false);
    const auto crystal = drawOf(device, look.countArt(PickupHud::crystalIcon(1)), false);
    REQUIRE(lastFrame.has_value());
    REQUIRE(card.has_value());
    REQUIRE(crystal.has_value());
    CHECK(*lastFrame < *card);
    CHECK(*card < *crystal);
    const auto top = static_cast<f32>(cardY + StatusBoxPainter::kBarHeight);
    const auto bottom = top + static_cast<f32>(StatusBoxPainter::kHeight);
    CHECK(test::minCorner(device.draws[*card]) == Vec2{128.0f, top});
    CHECK(test::maxCorner(device.draws[*card]) == Vec2{256.0f, bottom});
    CHECK(test::minCorner(device.draws[*crystal]).y == static_cast<f32>(countY));
    CHECK(test::minCorner(device.draws[*crystal]).x >= 256.0f);
    CHECK(test::maxCorner(device.draws[*crystal]).x <= 384.0f);
    look.clear();
    boxes.release();
}

TEST_CASE("the Dark Legacy look lends the art it draws with, once", "[dark-legacy-hud][assets]") {
    const auto root = test::assetOrSkip("STATIC/textures.ngc").parent_path().parent_path();
    test::assetOrSkip("INVENTORY/textures.ngc");
    test::FakeRenderDevice device;
    DarkLegacyHudPainter look;
    REQUIRE(look.load(device, root, nullptr));
    // The warrior's box with the first boss's key up, and a count over the third box.
    std::array<PlayerRuntime, 1> players;
    CharacterSave save;
    save.progress().health = 300;
    save.progress().relics.shards = 0x1;
    players[0].actor.spawn(0, save, nullptr, Vec3{0}, 0);
    PartyHud hud;
    hud.pickups().showCount(2, PickupHud::crystalIcon(1), 3, 15);
    const HudView view = viewOf(players, hud, {.keysShown = true});
    Canvas canvas;
    HudFrame frame{.canvas = canvas, .device = device, .darkLegacy = look};
    canvas.begin(device, Mat4{1});
    look.drawStatus(frame, view);
    canvas.end();
    const u32 created = device.texturesCreated;

    // Each lender gives the very texture drawn, and uploads nothing to do so, then or again.
    DarkLegacyHudArt& art = look;
    const Texture* panel = art.selectArt("S4_WAR");
    const Texture* key = art.staticArt("SM_KEY_BLU");
    const Texture* crystal = art.countArt(PickupHud::crystalIcon(1));
    REQUIRE(panel != nullptr);
    REQUIRE(key != nullptr);
    REQUIRE(crystal != nullptr);
    CHECK(drawOf(device, panel, false).has_value());
    CHECK(drawOf(device, key, false).has_value());
    CHECK(drawOf(device, crystal, false).has_value());
    CHECK(device.texturesCreated == created);
    CHECK(art.selectArt("S4_WAR") == panel);
    CHECK(art.staticArt("SM_KEY_BLU") == key);
    CHECK(art.countArt(PickupHud::crystalIcon(1)) == crystal);
    CHECK(art.staticArt("NO_SUCH_PICTURE") == nullptr);
    CHECK(device.texturesCreated == created);
    CHECK(art.smallCaps().ready());
    // INVENTORY is read on the first ask, and a picture uploaded on its first.
    const Texture* egg = art.inventoryArt("SHP_POJOEGG");
    REQUIRE(egg != nullptr);
    CHECK(device.texturesCreated == created + 1);
    CHECK(art.inventoryArt("SHP_POJOEGG") == egg);
    CHECK(art.inventoryArt("NO_SUCH_PICTURE") == nullptr);
    CHECK(device.texturesCreated == created + 1);
    // Cleared, it lends nothing.
    look.clear();
    CHECK(art.inventoryArt("SHP_POJOEGG") == nullptr);
    CHECK(art.staticArt("SM_KEY_BLU") == nullptr);
}

TEST_CASE("INVENTORY's pictures are read on the first ask and missed once", "[dark-legacy-hud]") {
    const auto root = plainRoot("dark-legacy-hud-inventory");
    test::FakeRenderDevice device;
    DarkLegacyHudPainter look;
    REQUIRE(look.load(device, root, nullptr));
    const u32 loaded = device.texturesCreated;
    DarkLegacyHudArt& art = look;
    // Without INVENTORY there is nothing, and it is not looked for again once it is there.
    CHECK(art.inventoryArt("SHP_POJOEGG") == nullptr);
    writePictures(root / "INVENTORY", {"SHP_POJOEGG"});
    CHECK(art.inventoryArt("SHP_POJOEGG") == nullptr);
    CHECK(device.texturesCreated == loaded);

    // A look loaded over it reads it on the first ask and uploads each picture once.
    DarkLegacyHudPainter fresh;
    REQUIRE(fresh.load(device, root, nullptr));
    const u32 opened = device.texturesCreated;
    const Texture* egg = fresh.inventoryArt("SHP_POJOEGG");
    REQUIRE(egg != nullptr);
    CHECK(device.texturesCreated == opened + 1);
    CHECK(fresh.inventoryArt("SHP_POJOEGG") == egg);
    CHECK(fresh.inventoryArt("SHP_MISSING") == nullptr);
    CHECK(device.texturesCreated == opened + 1);

    // Loaded again, the first look asks afresh.
    REQUIRE(look.load(device, root, nullptr));
    CHECK(art.inventoryArt("SHP_POJOEGG") != nullptr);
    // A look that never loaded has nothing to lend.
    DarkLegacyHudPainter unloaded;
    CHECK(unloaded.inventoryArt("SHP_POJOEGG") == nullptr);
    fresh.clear();
    look.clear();
}

} // namespace
