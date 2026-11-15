#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/MessageTable.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Progression.h"
#include "game/characters/Vitality.h"
#include "game/hud/BossMeter.h"
#include "game/hud/HealthMeter.h"
#include "game/hud/HelpMessages.h"
#include "game/hud/HudView.h"
#include "game/hud/PartyHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/level/ExitPortals.h"
#include "game/level/ImportedLevel.h"
#include "game/level/LevelCatalog.h"
#include "game/party/PartyNames.h"
#include "game/party/PlayerRuntime.h"

namespace {

using namespace gdl;
using namespace gdl::game;

CharacterSave member(std::string name, s32 character, s32 color) {
    CharacterSave save;
    save.name = std::move(name);
    save.character = character;
    save.color = color;
    return save;
}

/** The warrior's record, the one class the view's stats are read from here. */
ClassDataSet warriorOnly() {
    const auto dir = test::scratchDirectory("hud-view-classes");
    writeTextFile(dir / "WAR.json", R"({"fight": [600, 999], "speed": [350, 750],
        "armor": [300, 700], "magic": [100, 500]})");
    ClassDataSet classes;
    REQUIRE(classes.load(dir));
    return classes;
}

/** A level with an exit to G1 at (10, 0, 10) and one to G2 at (40, 0, 10), both radius 3. */
struct TwoExits {
    test::FakeRenderDevice device;
    WorldLayout layout;
    LevelCatalog catalog;
    ItemArchive items; ///< left unloaded: the portals work unseen
    ExitPortals portals;

    TwoExits() {
        const auto dir = test::scratchDirectory("hud-view-exits");
        std::filesystem::create_directories(dir / "wdata");
        writeTextFile(dir / "wdata/TOWN.json", R"({"realm": 7, "prefix": "levelG", "levels": [
          {"name": "G1", "title": "Fields"}, {"name": "G2", "title": "Farm"}]})");
        writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "GROUND", "position": [0, 0, 0], "next": -1, "child": -1}],
  "itemInfos": [{"type": 9, "subtype": 0, "name": "EXIT_PORTAL", "radius": 3, "height": 2}],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "position": [10, 0, 10], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 103, 49, 0, 0, 0, 0, 0, 0]},
    {"info": 0, "minPlayers": 1, "position": [40, 0, 10], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 103, 50, 0, 0, 0, 0, 0, 0]}]})");
        REQUIRE(layout.load(dir));
        REQUIRE(catalog.load(dir));
        REQUIRE(portals.bind(device, layout, AssetScope(items), catalog, nullptr));
        REQUIRE(portals.size() == 2);
    }
};

/** A stage's exits: an ordinary one tagged for G2 at (10, 0, 10), radius 3, and a secret one to
 * S1 at (40, 0, 10), radius 1. */
struct StageExits {
    test::FakeRenderDevice device;
    WorldLayout layout;
    LevelCatalog catalog;
    ItemArchive items; ///< left unloaded: the portals work unseen
    ExitPortals portals;

    StageExits() {
        const auto dir = test::scratchDirectory("hud-view-stage-exits");
        std::filesystem::create_directories(dir / "wdata");
        writeTextFile(dir / "wdata/TOWN.json", R"({"realm": 7, "prefix": "levelG", "levels": [
          {"name": "G1", "title": "Fields"}, {"name": "G2", "title": "Farm"}]})");
        writeTextFile(dir / "wdata/SECRET.json", R"({"realm": 12, "prefix": "levelS",
          "levels": [{"name": "S1", "title": "Secret"}]})");
        writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "GROUND", "position": [0, 0, 0], "next": -1, "child": -1}],
  "itemInfos": [
    {"type": 9, "subtype": 0, "name": "EXIT_PORTAL", "radius": 3, "height": 2},
    {"type": 9, "subtype": 50, "name": "SECRET_ICON", "radius": 1}],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "position": [10, 0, 10], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 103, 50, 0, 0, 0, 0, 0, 0]},
    {"info": 1, "minPlayers": 1, "position": [40, 0, 10], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 83, 49, 0, 0, 0, 0, 0, 0]}]})");
        REQUIRE(layout.load(dir));
        REQUIRE(catalog.load(dir));
        REQUIRE(portals.bind(device, layout, AssetScope(items), catalog, nullptr));
        REQUIRE(portals.size() == 2);
        REQUIRE_FALSE(portals.portal(0).secret);
        REQUIRE(portals.portal(1).secret);
        REQUIRE(portals.portal(1).destination.has_value());
    }
};

/** An imported level that answers only what every importer must. */
class PlainImported : public ImportedLevel {
public:
    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
};

/** One whose seat 1 waits on its exit to 0x0203 and whose seat 2 waits on an exit it names
 * no code for. */
class ExitingImported final : public PlainImported {
public:
    std::optional<ImportedExit> exitUnder(s32 seat) const override {
        if (seat == 1) {
            return ImportedExit{.code = 0x0203, .waiting = true};
        }
        if (seat == 2) {
            return ImportedExit{.code = std::nullopt, .waiting = true};
        }
        return std::nullopt;
    }
};

TEST_CASE("a seat's view holds what its box and the other looks need", "[game][hud][hud-view]") {
    const ClassDataSet classes = warriorOnly();
    CharacterSave save = member("ROWENA", 0, 2);
    save.progress().experience = 25000;
    save.progress().health = 700;
    save.progress().relics.legends = 0x0A2;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(2, save, nullptr, Vec3{0}, 0);
    players[0].turbo.add(60);
    players[0].turbo.step(45);
    players[0].itTicks = 7;
    players[0].exitWait.step(true, 3);
    const PowerupSelector selector;

    const HudSeat seat = hudSeatOf(2, players, selector, classes,
                                   {.keysShown = true, .quitOffered = false, .local = true});
    REQUIRE(seat.runtime == players.data());
    CHECK(seat.selector == &selector);
    // The box is the one the boxes always drew, with the frame's rules on it.
    const StatusBoxView box = PartyHud::status(2, players, &selector);
    CHECK(seat.box.active);
    CHECK(seat.box.name == box.name);
    CHECK(seat.box.health == box.health);
    CHECK(seat.box.color == box.color);
    CHECK(seat.box.keysShown);
    CHECK_FALSE(seat.box.quitOffered);
    CHECK(seat.local);
    // What only the other looks read, each from this seat's character.
    REQUIRE(players[0].turbo.shown() > 0);
    CHECK(seat.turbo == players[0].turbo.shown());
    const s32 level = experienceLevel(25000);
    CHECK(seat.mostHealth == mostHealth(level));
    CHECK(seat.mostHealth != seat.box.health);
    const StatBlock stats = characterStats(*classes.stats(0), players[0].actor.save());
    CHECK(stats.strength() > 0);
    CHECK(seat.stats.values == stats.values);
    CHECK(seat.experience == 25000);
    CHECK(seat.legends == 0x0A2);
    CHECK(seat.it);
    CHECK(seat.waitingAtExit);
    CHECK_FALSE(seat.departed);

    // The rules the other way round, and the seat neither tagged nor waiting.
    players[0].itTicks = 0;
    players[0].exitWait.step(false, 1);
    const HudSeat other = hudSeatOf(2, players, selector, classes,
                                    {.keysShown = false, .quitOffered = true, .local = false});
    CHECK_FALSE(other.box.keysShown);
    CHECK(other.box.quitOffered);
    CHECK_FALSE(other.local);
    CHECK_FALSE(other.it);
    CHECK_FALSE(other.waitingAtExit);
    // A class without its record reads no stats.
    players[0].actor.save().character = 1;
    CHECK(hudSeatOf(2, players, selector, classes, {}).stats.values == StatBlock{}.values);
}

TEST_CASE("an empty, departed or fallen seat shows only what its box shows", "[hud-view]") {
    const ClassDataSet classes;
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(1, member("ULRIC", 4, 3), nullptr, Vec3{0}, 0);
    players[0].actor.save().progress().health = 500;
    players[0].turbo.add(80);
    players[0].turbo.step(90);
    players[0].life = PlayerLife::InTower;
    players[1].actor.spawn(3, member("IVO", 2, 1), nullptr, Vec3{0}, 0);
    players[1].turbo.add(80);
    players[1].turbo.step(90);
    players[1].life = PlayerLife::InTower;
    players[1].towerPrompt = true;
    players[1].departed = true;
    const PowerupSelector selector;
    const HudSeatRules rules{.keysShown = true, .quitOffered = true, .local = true};

    const HudSeat empty = hudSeatOf(0, players, selector, classes, rules);
    CHECK(empty.runtime == nullptr);
    CHECK_FALSE(empty.box.active);
    CHECK_FALSE(empty.turbo);
    CHECK_FALSE(empty.departed);
    CHECK(empty.selector == &selector);
    // The frame's rules reach an empty box too, as they always did.
    CHECK(empty.box.keysShown);

    const HudSeat departed = hudSeatOf(3, players, selector, classes, rules);
    REQUIRE(departed.runtime == &players[1]);
    CHECK(departed.departed);
    CHECK_FALSE(departed.box.active);
    CHECK_FALSE(departed.turbo);
    CHECK(departed.runtime->actor.save().color == 1);
    // Gone from the game, it shows no turbo even were it on its feet.
    players[1].life = PlayerLife::Standing;
    const HudSeat departedStanding = hudSeatOf(3, players, selector, classes, rules);
    CHECK(departedStanding.departed);
    CHECK_FALSE(departedStanding.box.active);
    CHECK_FALSE(departedStanding.turbo);
    players[1].life = PlayerLife::InTower;

    const HudSeat fallen = hudSeatOf(1, players, selector, classes, rules);
    REQUIRE(fallen.runtime == players.data());
    CHECK(fallen.box.active);
    CHECK(fallen.box.inTower);
    CHECK(fallen.box.health == 0);
    CHECK_FALSE(fallen.turbo);
    CHECK_FALSE(fallen.departed);
    players[0].life = PlayerLife::Dying;
    const HudSeat dying = hudSeatOf(1, players, selector, classes, rules);
    CHECK(dying.box.health == 0);
    CHECK_FALSE(dying.box.inTower);
    CHECK_FALSE(dying.turbo);
    players[0].life = PlayerLife::Standing;
    CHECK(hudSeatOf(1, players, selector, classes, rules).turbo.has_value());
}

TEST_CASE("every seat's view comes from its own seat whatever the party's order", "[hud-view]") {
    const ClassDataSet classes;
    constexpr std::array<s32, 4> kOrder{2, 0, 3, 1};
    constexpr std::array<const char*, 4> kNames{"ZERO", "ONE", "TWO", "THREE"};
    std::array<PlayerRuntime, 4> players;
    for (usize index = 0; index < players.size(); ++index) {
        const s32 seat = kOrder.at(index);
        CharacterSave save = member(kNames.at(static_cast<usize>(seat)), seat, (seat + 1) % 4);
        save.progress().experience = 1000 * (seat + 1);
        save.progress().relics.legends = static_cast<u16>(1U << static_cast<u32>(seat + 1));
        players.at(index).actor.spawn(seat, save, nullptr, Vec3{0}, 0);
        players.at(index).itTicks = seat == 3 ? 1 : 0;
    }
    std::array<PowerupSelector, 4> selectors;
    for (usize seat = 0; seat < selectors.size(); ++seat) {
        CAPTURE(seat);
        const auto index = static_cast<s32>(seat);
        const HudSeat shown = hudSeatOf(index, players, selectors.at(seat), classes, {});
        REQUIRE(shown.runtime != nullptr);
        CHECK(shown.runtime->actor.player() == index);
        CHECK(shown.box.name == kNames.at(seat));
        CHECK(shown.box.classIndex == index);
        CHECK(shown.experience == 1000 * (index + 1));
        CHECK(shown.legends == (1U << (seat + 1)));
        CHECK(shown.it == (seat == 3));
        CHECK(shown.selector == &selectors.at(seat));
    }
}

TEST_CASE("a seat's Dark Legacy exit is named by its own seat, not its party place", "[hud-view]") {
    const TwoExits level;
    // Party places 0, 1 and 2 are seats 2, 0 and 3.
    std::array<PlayerRuntime, 3> players;
    players[0].actor.spawn(2, member("ODO", 0, 0), nullptr, Vec3{40, 0, 10}, 0);
    players[1].actor.spawn(0, member("ULRIC", 4, 3), nullptr, Vec3{10, 0, 10}, 0);
    players[2].actor.spawn(3, member("IVO", 2, 1), nullptr, Vec3{15.25f, 0, 10}, 0);
    std::array<PortalVisitor, 3> standing;
    for (usize index = 0; index < standing.size(); ++index) {
        standing.at(index) = PortalVisitor{.position = players.at(index).actor.position(),
                                           .radius = 0.75f,
                                           .partyIndex = static_cast<s32>(index)};
    }
    // The tower's portals lead to the levels they name.
    const LevelRef tower = LevelRef::tower();
    const auto pads = darkLegacyPadsOf(level.portals, tower, standing, players);

    REQUIRE(pads[2].has_value());
    CHECK(pads[2]->identity == kDarkLegacyPadIdentityBase + 1);
    CHECK_FALSE(pads[2]->nativeCode.has_value());
    REQUIRE(pads[2]->destination != nullptr);
    CHECK(pads[2]->destination == &*level.portals.portal(1).destination);
    CHECK(pads[2]->destination->name == "G2");
    REQUIRE(pads[0].has_value());
    CHECK(pads[0]->identity == kDarkLegacyPadIdentityBase);
    REQUIRE(pads[0]->destination != nullptr);
    CHECK(pads[0]->destination->name == "G1");
    // 5.25 off G1's exit, past 3 + 0.75 but within the two more a party of three is given.
    REQUIRE(pads[3].has_value());
    CHECK(pads[3]->identity == kDarkLegacyPadIdentityBase);
    CHECK_FALSE(pads[1].has_value()); // nobody plays seat 1

    // Alone, the same player is off the exit; a visitor naming no party place is nobody's.
    const std::array alone{
        standing[2], PortalVisitor{.position = Vec3{10, 0, 10}, .radius = 0.75f, .partyIndex = 7}};
    const auto fewer = darkLegacyPadsOf(level.portals, tower, std::span{alone}.first(1), players);
    CHECK_FALSE(fewer[3].has_value());
    for (const auto& pad :
         darkLegacyPadsOf(level.portals, tower, std::span{alone}.last(1), players)) {
        CHECK_FALSE(pad.has_value());
    }
    for (const auto& pad : darkLegacyPadsOf(level.portals, tower, {}, players)) {
        CHECK_FALSE(pad.has_value());
    }
}

TEST_CASE("a Dark Legacy exit names where it takes the party: the tower from an ordinary stage, "
          "else the level it names",
          "[hud-view]") {
    const StageExits level;
    // Party place 0 is seat 3, on the stage's exit; place 1 is seat 1, on the secret one.
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, member("ODO", 0, 0), nullptr, Vec3{10, 0, 10}, 0);
    players[1].actor.spawn(1, member("IVO", 2, 1), nullptr, Vec3{40, 0, 10}, 0);
    std::array<PortalVisitor, 2> standing;
    for (usize index = 0; index < standing.size(); ++index) {
        standing.at(index) = PortalVisitor{.position = players.at(index).actor.position(),
                                           .radius = 0.75f,
                                           .partyIndex = static_cast<s32>(index)};
    }
    const ExitPortals::Portal& stageExit = level.portals.portal(0);
    const ExitPortals::Portal& secretExit = level.portals.portal(1);
    const auto fields = level.catalog.byName("G1");
    REQUIRE(fields.has_value());

    // An ordinary stage's exit takes the party back to the tower, whatever its tag names; its
    // secret exit goes where it says.
    const auto fromStage = darkLegacyPadsOf(level.portals, *fields, standing, players);
    REQUIRE(fromStage[3].has_value());
    CHECK(fromStage[3]->identity == kDarkLegacyPadIdentityBase);
    REQUIRE(fromStage[3]->destination != nullptr);
    CHECK(fromStage[3]->destination->isTower());
    REQUIRE(fromStage[1].has_value());
    CHECK(fromStage[1]->identity == kDarkLegacyPadIdentityBase + 1);
    CHECK(fromStage[1]->destination == &*secretExit.destination);
    CHECK(fromStage[1]->destination->name == "S1");

    // The tower's portals, and the Temple's and Underworld's first stages, which go straight on
    // to their bosses, lead to the level the exit names.
    LevelRef firstTemple;
    firstTemple.realmId = 5;
    LevelRef firstUnderworld;
    firstUnderworld.realmId = 6;
    const std::array<LevelRef, 3> leadOn{LevelRef::tower(), firstTemple, firstUnderworld};
    for (const LevelRef& here : leadOn) {
        CAPTURE(here.realmId);
        const auto pads = darkLegacyPadsOf(level.portals, here, standing, players);
        REQUIRE(pads[3].has_value());
        CHECK(pads[3]->destination == &*stageExit.destination);
        CHECK(pads[3]->destination->name == "G2");
        REQUIRE(pads[1].has_value());
        CHECK(pads[1]->destination == &*secretExit.destination);
    }
    // The Temple's later stages go back to the tower like any other.
    LevelRef laterTemple = firstTemple;
    laterTemple.index = 1;
    const auto fromLater = darkLegacyPadsOf(level.portals, laterTemple, standing, players);
    REQUIRE(fromLater[3].has_value());
    CHECK(fromLater[3]->destination->isTower());
}

TEST_CASE("an imported level names no exit, no wait and no native look unless it says so; one "
          "that does reaches the seat it names",
          "[hud-view]") {
    const PlainImported plain{};
    CHECK(plain.hudStyleId().empty());
    for (const auto& exit : importedExitsOf(plain)) {
        CHECK_FALSE(exit.has_value());
    }

    const ExitingImported exiting{};
    const auto exits = importedExitsOf(exiting);
    CHECK_FALSE(exits[0].has_value());
    REQUIRE(exits[1].has_value());
    CHECK(exits[1]->code == 0x0203U);
    CHECK(exits[1]->waiting);
    REQUIRE(exits[2].has_value());
    CHECK_FALSE(exits[2]->code.has_value());
    CHECK(exits[2]->waiting);
    CHECK_FALSE(exits[3].has_value());

    // An exit that names its destination is a pad of its own code, clear of Dark Legacy's.
    const auto pad = importedPadOf(*exits[1]);
    REQUIRE(pad.has_value());
    CHECK(pad->identity == 0x0203U);
    CHECK(pad->nativeCode == 0x0203U);
    CHECK(pad->destination == nullptr);
    CHECK(pad->identity < kDarkLegacyPadIdentityBase);
    CHECK_FALSE(importedPadOf(*exits[2]).has_value());
    CHECK_FALSE(importedPadOf(ImportedExit{.code = 0, .waiting = false}).has_value());
}

TEST_CASE("boss readings are filtered and capped as the meters bind them", "[hud-view]") {
    const auto reading = [](bool shown, s32 pieces, f32 health, f32 maxHealth) {
        HealthMeterReading result;
        result.definition.shown = shown;
        result.definition.pieces = pieces;
        result.definition.backed = true;
        result.definition.advance = 256;
        result.health = health;
        result.maxHealth = maxHealth;
        return result;
    };
    const std::vector<HealthMeterReading> readings{
        reading(true, 2, 100, 200), reading(false, 1, 1, 2), reading(true, 0, 3, 4),
        reading(true, 1, 30, 40),   reading(true, 1, 5, 6),  reading(true, 1, 7, 8)};
    std::array<HudBossReading, BossMeters::kCapacity> out{};
    out.fill({.health = -1, .maxHealth = -1});
    const usize count = hudBossReadingsOf(readings, out);
    REQUIRE(count == BossMeters::kCapacity);
    CHECK(out[0].health == 100);
    CHECK(out[0].maxHealth == 200);
    CHECK(out[1].health == 30);
    CHECK(out[1].maxHealth == 40);
    CHECK(out[2].health == 5);
    CHECK(out[2].maxHealth == 6);
    // The meters bound from the same readings stand for the same bodies, in the same order.
    BossMeters meters;
    meters.bind(readings, nullptr);
    REQUIRE(meters.count() == count);
    for (usize index = 0; index < count; ++index) {
        CHECK(meters.meter(index).shown() == out.at(index).health);
    }
    // Fewer than the meters hold leave the rest empty.
    const std::vector<HealthMeterReading> one{reading(false, 2, 9, 9), reading(true, 1, 11, 12)};
    REQUIRE(hudBossReadingsOf(one, out) == 1);
    CHECK(out[0].health == 11);
    CHECK(out[1].health == 0);
    CHECK(out[2].maxHealth == 0);
    CHECK(hudBossReadingsOf({}, out) == 0);
}

TEST_CASE("help sits on its player's head, or the canvas centre", "[hud-view]") {
    const auto root = test::scratchDirectory("hud-view-help");
    writeTextFile(root / "messages.json",
                  R"({"messages":[{"name":"LEVELUP","lines":["LEVEL %d"]}]})");
    MessageTable texts;
    REQUIRE(texts.load(root / "messages.json"));
    HelpMessages help;
    help.setTexts(&texts);
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(1, member("ODO", 5, 2), nullptr, Vec3{0}, 0);
    players[1].actor.spawn(3, member("IVO", 2, 0), nullptr, Vec3{0}, 0);
    const Mat4 canvasProjection =
        makeVirtualScreenTransform(makeScreenProjection(640, 448), 512, 384, 640, 448);
    // The clip transform is the canvas's own, so a point lies where its x and y say.
    const Mat4& clip = canvasProjection;
    const auto at = [&](std::optional<Vec3> anchor) {
        return hudHelpOf(help, anchor, players, clip, canvasProjection, 512, 384);
    };
    CHECK_FALSE(at(Vec3{100, 200, 0}).has_value()); // nothing is up

    REQUIRE(help.post(HelpMessages::kLevelUp, 3, {}, 12) != nullptr);
    const Vec3 head{100, 200, 0};
    const auto over = at(head);
    REQUIRE(over.has_value());
    CHECK(over->anchored);
    CHECK(over->head == PartyNames::screenOf(clip, head, canvasProjection).value());
    CHECK(over->player == 3);
    CHECK(over->color == 0); // the colour of the seat's own player, not of the first in the party
    REQUIRE(over->lines.size() == 1);
    CHECK(over->lines[0] == "LEVEL 12");
    const auto centred = at(std::nullopt);
    REQUIRE(centred.has_value());
    CHECK_FALSE(centred->anchored);
    CHECK(centred->head == Vec2{256, 192});
    CHECK(centred->color == 0);
    // An anchor behind the eye does not project, and the message keeps to the canvas centre.
    Mat4 behind{1.0f};
    behind[3][3] = -1.0f;
    REQUIRE_FALSE(PartyNames::screenOf(behind, head, canvasProjection).has_value());
    const auto unseen = hudHelpOf(help, head, players, behind, canvasProjection, 512, 384);
    REQUIRE(unseen.has_value());
    CHECK_FALSE(unseen->anchored);
    CHECK(unseen->head == Vec2{256, 192});
    CHECK(unseen->player == 3);

    // A message at a place is for nobody and wears no seat's colour.
    help.clear();
    REQUIRE(help.post(HelpMessages::kLevelUp, -1, {}, 3) != nullptr);
    const auto placed = at(std::nullopt);
    REQUIRE(placed.has_value());
    CHECK(placed->player == -1);
    CHECK(placed->color == -1);
    // A seat nobody plays has no colour either.
    help.clear();
    REQUIRE(help.post(HelpMessages::kLevelUp, 2, {}, 4) != nullptr);
    CHECK(at(head)->color == -1);
}

} // namespace
