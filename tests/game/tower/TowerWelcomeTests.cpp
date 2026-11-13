#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"
#include "engine/world/WorldAnimator.h"

#include "FakeGameServices.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Party.h"
#include "game/characters/Relics.h"
#include "game/hud/LevelMessages.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelTriggers.h"
#include "game/level/TowerAccess.h"
#include "game/party/PlayerRuntime.h"
#include "game/tower/LevelBeaten.h"
#include "game/tower/TowerRelics.h"
#include "game/tower/TowerWelcome.h"
#include "game/widgets/ScrollBox.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** GarmMessage's scale in the table below, unlike the welcome's. */
constexpr f32 kGarmScale = 2.0f;
/** The game's own words for Sumner's gateways in the table below. */
constexpr std::string_view kGatewayPage = "G\nGG\nA";

/** The tower's arrival scrolls in a table of their own, with the art to show them (no burn
 * frames, so a scroll read through closes at once), and the game's text for the gateways. */
struct ArrivalScrolls {
    test::FakeRenderDevice device;
    TextureSet textures;
    LevelMessages messages;
    StringTable strings;
    LevelWorld world;
    SumnerFigure sumner;

    ArrivalScrolls() {
        const auto root = test::scratchDirectory("tower-arrival-scrolls");
        std::filesystem::create_directories(root / "STATIC");
        std::filesystem::create_directories(root / "fonts");
        std::filesystem::create_directories(root / "text");
        std::filesystem::create_directories(root / "strings");
        writeFile(root / "STATIC/tile.png", test::kTinyPng);
        writeTextFile(root / "STATIC/textures.json", R"({"bitmaps": [
            {"index": 0, "name": "FONT32", "file": "tile.png", "width": 2, "height": 2},
            {"index": 1, "name": "SCROLL_A", "file": "tile.png", "width": 2, "height": 2},
            {"index": 2, "name": "FONT32_GLOW", "file": "tile.png", "width": 2, "height": 2}]})");
        writeTextFile(root / "fonts/font32.json", R"({"height": 10, "glyphs": [
            {"code": 65, "width": 6, "x": 0, "y": 0},
            {"code": 71, "width": 6, "x": 0, "y": 0}]})");
        writeTextFile(root / "text/scroll_e.json", R"({"fonts": ["font32"], "messages": [
            {"name": "WELCOMEMESSAGE", "font": 0, "scale": 1, "lines": ["A", "AA"]},
            {"name": "GARMMESSAGE", "font": 0, "scale": 2, "lines": ["G", "GG"]}], "lists": []})");
        writeTextFile(root / "strings/en.json",
                      R"({"gateway.announcement": "G\nGG\nA", "scroll.pressButton": "A"})");
        messages.load(device, textures, root, nullptr);
        REQUIRE(strings.load(root / "strings", "en"));
    }

    /** The message on the scroll, by its name in the table; none while no scroll is up. */
    std::optional<std::string> shown() const {
        const auto look = messages.look();
        return look ? std::optional{messages.strings().message(look->message).name} : std::nullopt;
    }
    TowerWelcome::Due arrive(TowerWelcome& welcome) {
        return welcome.arrived(device, messages, &strings, world.layout(), sumner);
    }
    TowerWelcome::Due close(TowerWelcome& welcome) {
        return welcome.scrollClosed(device, messages, &strings, world.layout(), sumner);
    }
    /** Whether the scroll up is the gateways' page, at GarmMessage's scale. */
    bool showsGateways() const {
        const ScrollBox& scroll = messages.scroll();
        return scroll.active() && !shown() && scroll.pageCount() == 1 &&
               scroll.lines() == ScrollBox::splitLines(kGatewayPage) &&
               scroll.area() == scroll.layout(kGatewayPage, kGarmScale, "A").area;
    }
    /** Turns every page as soon as it may be turned. */
    void readThrough() {
        for (s32 page = 0; page < 8 && messages.active(); ++page) {
            messages.step(ScrollBox::kHoldTicks, 0);
            messages.step(1, 1);
        }
    }
};

TEST_CASE("a party back from the Underworld's throne arrives to GarmMessage and no cut",
          "[game][screens][welcome]") {
    ArrivalScrolls f;
    REQUIRE(f.messages.text().ready());
    TowerWelcome welcome;
    welcome.open(f.world, false, LevelBeaten{6, 1});
    CHECK(welcome.due() == TowerWelcome::Due::Garm);
    CHECK_FALSE(f.messages.active()); // nothing shows while the party materialises
    f.arrive(welcome);
    REQUIRE(f.shown() == "GARMMESSAGE");
    CHECK(f.messages.scroll().pageCount() == 2);
    CHECK(f.messages.scroll().lines() == std::vector<std::string>{"G"});
    CHECK(welcome.due() == TowerWelcome::Due::Nothing);
    CHECK(welcome.intro() == TowerWelcome::Intro::None);
    // Read through, it leaves the tower as it was: no cut to the crystals follows it.
    f.readThrough();
    REQUIRE_FALSE(f.messages.active());
    CHECK(f.close(welcome) == TowerWelcome::Due::Nothing);
    CHECK(welcome.intro() == TowerWelcome::Intro::None);
    CHECK_FALSE(welcome.camera().has_value());
    CHECK_FALSE(welcome.hold(1));
    // It is shown once an arrival.
    f.arrive(welcome);
    CHECK_FALSE(f.messages.active());
    welcome.open(f.world, false, LevelBeaten{6, 1});
    welcome.clear();
    CHECK(welcome.due() == TowerWelcome::Due::Nothing);
}

TEST_CASE("no other level beaten brings a scroll, and a new party's welcome goes alone",
          "[game][screens][welcome]") {
    ArrivalScrolls f;
    REQUIRE(f.messages.text().ready());
    // The Underworld's gates, Garm's citadel, the temple's second stage, another realm's second.
    for (const std::optional<LevelBeaten> beaten :
         {std::optional<LevelBeaten>{}, std::optional{LevelBeaten{6, 0}},
          std::optional{LevelBeaten{8, 3}}, std::optional{LevelBeaten{5, 1}},
          std::optional{LevelBeaten{1, 1}}}) {
        CAPTURE(beaten.has_value(), beaten.value_or(LevelBeaten{}).realm,
                beaten.value_or(LevelBeaten{}).level);
        TowerWelcome welcome;
        welcome.open(f.world, false, beaten);
        CHECK(welcome.due() == TowerWelcome::Due::Nothing);
        f.arrive(welcome);
        CHECK_FALSE(f.messages.active());
        CHECK(welcome.intro() == TowerWelcome::Intro::None);
    }
    // A party welcomed comes to the tower for the first time, after no level: the welcome is
    // all it is shown, whatever it is said to have beaten.
    TowerWelcome welcome;
    welcome.open(f.world, true, LevelBeaten{6, 1});
    CHECK(welcome.due() == TowerWelcome::Due::Welcome);
    f.arrive(welcome);
    REQUIRE(f.shown() == "WELCOMEMESSAGE");
    CHECK(welcome.intro() == TowerWelcome::Intro::Scroll);
    f.readThrough();
    REQUIRE_FALSE(f.messages.active());
    CHECK(f.close(welcome) == TowerWelcome::Due::Nothing);
    CHECK(welcome.intro() == TowerWelcome::Intro::Done); // no crystal marker in an empty world
    f.arrive(welcome);
    CHECK_FALSE(f.messages.active());
}

TEST_CASE("a party owed word of the gateways arrives to its one page, drawn as GarmMessage is",
          "[game][screens][welcome][gateways]") {
    ArrivalScrolls f;
    REQUIRE(f.messages.text().ready());
    TowerWelcome welcome;
    welcome.open(f.world, false, std::nullopt, true);
    CHECK(welcome.due() == TowerWelcome::Due::Gateways);
    CHECK_FALSE(f.messages.active()); // nothing shows while the party materialises
    CHECK(f.arrive(welcome) == TowerWelcome::Due::Gateways);
    // The game's own words, none of the rom's messages, at GarmMessage's scale.
    CHECK(f.showsGateways());
    CHECK_FALSE(f.messages.scroll().area() ==
                f.messages.scroll().layout(kGatewayPage, 1.0f, "A").area);
    CHECK(welcome.due() == TowerWelcome::Due::Nothing);
    CHECK(welcome.intro() == TowerWelcome::Intro::None);
    // Read through, nothing follows it, no cut and no other scroll; it is shown once an arrival.
    f.readThrough();
    REQUIRE_FALSE(f.messages.active());
    CHECK(f.close(welcome) == TowerWelcome::Due::Nothing);
    CHECK_FALSE(f.messages.active());
    CHECK(welcome.intro() == TowerWelcome::Intro::None);
    CHECK_FALSE(welcome.camera().has_value());
    CHECK(f.arrive(welcome) == TowerWelcome::Due::Nothing);
    CHECK_FALSE(f.messages.active());
}

TEST_CASE("back from the Underworld's throne, a party owed word of the gateways hears it next",
          "[game][screens][welcome][gateways]") {
    ArrivalScrolls f;
    REQUIRE(f.messages.text().ready());
    TowerWelcome welcome;
    welcome.open(f.world, false, LevelBeaten{6, 1}, true);
    CHECK(welcome.due() == TowerWelcome::Due::Garm);
    CHECK(f.arrive(welcome) == TowerWelcome::Due::Garm);
    REQUIRE(f.shown() == "GARMMESSAGE");
    CHECK(welcome.due() == TowerWelcome::Due::Gateways); // once GarmMessage has gone
    f.readThrough();
    REQUIRE_FALSE(f.messages.active());
    CHECK(f.close(welcome) == TowerWelcome::Due::Gateways);
    CHECK(f.showsGateways());
    CHECK(welcome.due() == TowerWelcome::Due::Nothing);
    f.readThrough();
    REQUIRE_FALSE(f.messages.active());
    CHECK(f.close(welcome) == TowerWelcome::Due::Nothing);
    CHECK_FALSE(f.messages.active());
    CHECK(welcome.intro() == TowerWelcome::Intro::None);
}

TEST_CASE("a new party's welcome goes alone, even to a party owed word of the gateways",
          "[game][screens][welcome][gateways]") {
    ArrivalScrolls f;
    REQUIRE(f.messages.text().ready());
    TowerWelcome welcome;
    welcome.open(f.world, true, LevelBeaten{6, 1}, true);
    CHECK(welcome.due() == TowerWelcome::Due::Welcome);
    CHECK(f.arrive(welcome) == TowerWelcome::Due::Welcome);
    REQUIRE(f.shown() == "WELCOMEMESSAGE");
    f.readThrough();
    REQUIRE_FALSE(f.messages.active());
    CHECK(f.close(welcome) == TowerWelcome::Due::Nothing); // on to the cut instead
    CHECK(welcome.intro() == TowerWelcome::Intro::Done);   // no crystal marker in an empty world
    CHECK_FALSE(f.messages.active());
    CHECK(f.close(welcome) == TowerWelcome::Due::Nothing);
    CHECK_FALSE(f.messages.active());
}

TEST_CASE("word of the gateways that cannot be shown waits for a later arrival",
          "[game][screens][welcome][gateways]") {
    ArrivalScrolls f;
    REQUIRE(f.messages.text().ready());
    StringTable without;
    const auto other = test::scratchDirectory("tower-arrival-scrolls-other-words");
    writeTextFile(other / "en.json", R"({"scroll.pressButton": "A"})");
    REQUIRE(without.load(other, "en"));
    const std::array<const StringTable*, 2> tables{nullptr, &without};
    for (const StringTable* strings : tables) {
        CAPTURE(strings == nullptr);
        // Without its words nothing opens; the party has not been told, so nothing is lost.
        TowerWelcome welcome;
        welcome.open(f.world, false, std::nullopt, true);
        CHECK(welcome.arrived(f.device, f.messages, strings, f.world.layout(), f.sumner) ==
              TowerWelcome::Due::Nothing);
        CHECK_FALSE(f.messages.active());
        // GarmMessage still shows, and nothing follows it.
        welcome.open(f.world, false, LevelBeaten{6, 1}, true);
        CHECK(welcome.arrived(f.device, f.messages, strings, f.world.layout(), f.sumner) ==
              TowerWelcome::Due::Garm);
        REQUIRE(f.shown() == "GARMMESSAGE");
        f.readThrough();
        REQUIRE_FALSE(f.messages.active());
        CHECK(welcome.scrollClosed(f.device, f.messages, strings, f.world.layout(), f.sumner) ==
              TowerWelcome::Due::Nothing);
        CHECK_FALSE(f.messages.active());
    }
}

TEST_CASE("a party is owed word of the gateways while one of it has beaten Garm untold",
          "[game][screens][welcome][gateways]") {
    test::FixedGateways gateways;
    gateways.offered = {{0, "a", false}, {1, "b", true}};
    CharacterSave garm;
    garm.progress().relics.addShard(LevelRef::orderOf(TowerAccess::kBattlefieldWorld));
    CharacterSave other;
    other.progress().relics.shards = Relics::kAllShards; // every boss's piece but his
    std::vector<PartyMember> party{PartyMember{0, other}};
    // Nobody has beaten Garm: nothing is owed, and nothing is looked for on disk.
    CHECK_FALSE(TowerWelcome::owesGateways(party, &gateways));
    CHECK(gateways.asks == 0);
    // One has: owed while a world past the gateways is usable here.
    party.push_back(PartyMember{1, garm});
    CHECK(TowerWelcome::owesGateways(party, &gateways));
    CHECK(gateways.asks == 1);
    gateways.offered[1].usable = false;
    CHECK_FALSE(TowerWelcome::owesGateways(party, &gateways));
    gateways.offered[0].usable = true;
    CHECK(TowerWelcome::owesGateways(party, &gateways));
    // Without the link, as online, never, and nothing is looked for.
    const u32 asked = gateways.asks;
    CHECK_FALSE(TowerWelcome::owesGateways(party, nullptr));
    // Told, never again, whatever is installed later.
    party[1].save.gatewaysAnnounced = true;
    CHECK_FALSE(TowerWelcome::owesGateways(party, &gateways));
    CHECK(gateways.asks == asked);
    // Garm's piece won with another class counts only while the character plays it.
    CharacterSave switched = garm;
    switched.selectClass(2);
    CHECK_FALSE(TowerWelcome::owesGateways(std::vector{PartyMember{0, switched}}, &gateways));
}

TEST_CASE("telling of the gateways marks those that beat Garm, as they are and as they came",
          "[game][screens][welcome][gateways]") {
    CharacterSave garm;
    garm.progress().relics.addShard(LevelRef::orderOf(TowerAccess::kBattlefieldWorld));
    std::vector<PlayerRuntime> players(3);
    players[0].actor.spawn(0, garm, nullptr, Vec3{0.0f}, 0.0f);
    players[1].actor.spawn(1, CharacterSave{}, nullptr, Vec3{2.0f, 0.0f, 0.0f}, 0.0f);
    players[2].actor.spawn(2, garm, nullptr, Vec3{4.0f, 0.0f, 0.0f}, 0.0f);
    players[2].departed = true; // gone from the party
    for (PlayerRuntime& runtime : players) {
        runtime.entrySave = runtime.actor.save();
    }
    CHECK(TowerWelcome::tellGateways(players) == 1);
    CHECK(players[0].actor.save().gatewaysAnnounced);
    CHECK(players[0].entrySave.gatewaysAnnounced); // kept by a level given up
    CHECK_FALSE(players[1].actor.save().gatewaysAnnounced);
    CHECK_FALSE(players[1].entrySave.gatewaysAnnounced);
    CHECK_FALSE(players[2].actor.save().gatewaysAnnounced);
    // Told again, nobody is told for the first time.
    CHECK(TowerWelcome::tellGateways(players) == 0);
    CHECK(players[0].actor.save().gatewaysAnnounced);
}

TEST_CASE("Sumner's word of his gateways fits his scroll as GarmMessage's pages do",
          "[game][screens][welcome][gateways][assets]") {
    const auto root = test::assetOrSkip("FONTS/font32.fnt").parent_path().parent_path();
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    test::FakeRenderDevice device;
    TextureSet textures;
    LevelMessages messages;
    messages.load(device, textures, root, &strings);
    REQUIRE(messages.text().ready());
    LevelWorld world;
    SumnerFigure sumner;
    TowerWelcome welcome;
    welcome.open(world, false, std::nullopt, true);
    REQUIRE(welcome.arrived(device, messages, &strings, world.layout(), sumner) ==
            TowerWelcome::Due::Gateways);
    const auto garm = messages.strings().find("GARMMESSAGE");
    REQUIRE(garm.has_value());
    const f32 scale = messages.strings().message(*garm).scale;
    // One page of the user's words, every one of them kept, in lines the font draws whole
    // within the scroll's room, on a scroll that stays on the screen.
    const ScrollBox& scroll = messages.scroll();
    CHECK(scroll.pageCount() == 1);
    CHECK(scroll.lines().size() <= 9);
    std::string words;
    for (const std::string& line : scroll.lines()) {
        CAPTURE(line);
        CHECK(messages.text().measure(line, scale) <= ScrollBox::kMaxWidth - ScrollBox::kMargin);
        for (const char c : line) {
            CHECK((c == ' ' || messages.text().font()->glyph(static_cast<u8>(c)) != nullptr));
        }
        words += words.empty() ? line : " " + line;
    }
    CHECK(words == "Garm is no more! My old gateways have woken. Speak to me to travel through "
                   "the Legacy of Legends.");
    CHECK(scroll.area() == scroll
                               .layout(strings.get(TowerWelcome::kGatewayScroll), scale,
                                       strings.get("scroll.pressButton"))
                               .area);
    CHECK(scroll.area().y >= 0.0f);
}

TEST_CASE("Sumner's approach pad and the completed Temple window own different light rays",
          "[welcome][tower-relics][tower-lights][assets]") {
    const bool completeWindow = GENERATE(false, true);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    const auto& objects = world.layout().objects();
    const auto objectOf = [&](std::string_view name) {
        for (usize i = 0; i < objects.size(); ++i) {
            if (objects[i].name == name) {
                return i;
            }
        }
        FAIL("Missing tower light object: " << name);
        return usize{0};
    };
    const usize window = objectOf("L1XPLIGHTRAY01");
    const std::array beams{objectOf("L1XPLOWERLIGHTR"), objectOf("L1XPUPPERLIGHTR")};
    const usize lectern = objectOf("L1DRAWB69");
    for (const usize beam : beams) {
        REQUIRE(objects[beam].parent == static_cast<s32>(lectern));
    }
    REQUIRE(objects[window].parent != static_cast<s32>(lectern));
    Vec3 spot{0};
    bool found = false;
    for (usize i = 0; i < world.triggers().size(); ++i) {
        const auto& trigger = world.triggers().trigger(i);
        if (trigger.id == 40) {
            REQUIRE(trigger.target == static_cast<s32>(lectern));
            REQUIRE(trigger.nextId == 41); // the podium light follows the same pad
            spot = trigger.spot;
            found = true;
        }
    }
    REQUIRE(found);
    world.startTriggers({});
    TowerWelcome welcome;
    welcome.open(world, false);
    std::array<Relics, 1> collections;
    if (completeWindow) {
        collections[0].shards = 0x1FE;
    }
    TowerRelics relics;
    relics.begin(collections, MessageTable{});
    relics.bind(device, world, spot);
    const f32 windowAlpha = completeWindow ? 1.0f : 0.0f;
    CHECK(world.objectAlpha(window) == windowAlpha);
    for (const usize beam : beams) {
        CHECK(world.objectAlpha(beam) == 0);
    }
    std::array<TriggerVisitor, 1> visitors;
    visitors[0].position = spot;
    // Enter and leave twice: neither the collection nor its presentation may
    // override the pad, and the pad must never reveal or dim the window beam.
    for (s32 visit = 0; visit < 2; ++visit) {
        for (s32 frame = 0; frame < 120; ++frame) {
            world.updateTriggers(1.0f / 30.0f, visitors);
            relics.animate(1.0f / 30.0f);
            CHECK(world.objectAlpha(window) == windowAlpha);
        }
        for (const usize beam : beams) {
            CHECK(world.objectAlpha(beam) == Catch::Approx(1));
        }
        for (s32 frame = 0; frame < 120; ++frame) {
            world.updateTriggers(1.0f / 30.0f, {});
            relics.animate(1.0f / 30.0f);
            CHECK(world.objectAlpha(window) == windowAlpha);
        }
        for (const usize beam : beams) {
            CHECK(world.objectAlpha(beam) == Catch::Approx(0).margin(0.001f));
        }
    }
}

TEST_CASE("only a standing character visits Sumner's spot", "[game][screens][welcome]") {
    const auto dir = test::sampleLevel("sumner-spot-visitors");
    writeTextFile(dir / "world.json", R"({"objects":[{"name":"FLOOR","position":[0,0,0]}],
      "itemInfos":[{"type":5,"subtype":24,"name":"SPOT","radius":1}],
      "itemInstances":[{"info":0,"position":[0,0,0],"params":[255,255,0,0,4,255,240,0,0,0,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    WorldAnimator animator;
    animator.bind(layout);
    LevelTriggers triggers;
    triggers.bind(layout, animator, nullptr);
    REQUIRE(triggers.size() == 1);
    REQUIRE(triggers.trigger(0).id == TowerWelcome::kSumnerSpot);
    std::vector<PlayerRuntime> players(2);
    players[0].actor.spawn(1, {}, nullptr, Vec3{30, 0, 0}, 0);
    players[1].actor.spawn(3, {}, nullptr, Vec3{0, 0, 0}, 0);
    REQUIRE(TowerWelcome::visitorOf(triggers, players) == 3);
    players[1].life = PlayerLife::InTower; // a dropped peer's seat, left where it stood
    CHECK_FALSE(TowerWelcome::visitorOf(triggers, players).has_value());
    players[0].actor.place(Vec3{0, 0, 0});
    CHECK(TowerWelcome::visitorOf(triggers, players) == 1);
}

TEST_CASE("a party is new to the tower while none of its classes has experience",
          "[game][screens][welcome]") {
    CHECK_FALSE(TowerWelcome::freshParty({}));
    std::vector<PartyMember> party{PartyMember{0, CharacterSave{}},
                                   PartyMember{1, CharacterSave{}}};
    CHECK(TowerWelcome::freshParty(party));
    party[1].save.classes[3].experience = 1;
    CHECK_FALSE(TowerWelcome::freshParty(party));
}

TEST_CASE("without its scroll or its camera the welcome passes straight on",
          "[game][screens][welcome]") {
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelMessages messages;
    SumnerFigure sumner;
    TowerWelcome welcome;
    welcome.open(world, true);
    CHECK(welcome.intro() == TowerWelcome::Intro::None);
    CHECK_FALSE(welcome.hold(1));
    welcome.arrived(device, messages, nullptr, world.layout(), sumner);
    CHECK(welcome.intro() == TowerWelcome::Intro::Done);
    CHECK_FALSE(welcome.camera().has_value());
    CHECK_FALSE(welcome.hold(1));
    // Once over, a second arrival starts nothing.
    welcome.arrived(device, messages, nullptr, world.layout(), sumner);
    CHECK(welcome.intro() == TowerWelcome::Intro::Done);
    welcome.clear();
    CHECK(welcome.intro() == TowerWelcome::Intro::None);
}

TEST_CASE("the welcome crystal cut holds its ticks", "[game][screens][welcome][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    SumnerFigure sumner;
    REQUIRE(sumner.load(device, world.items(), world.layout()));

    // Without the scroll, the welcome cuts to the crystals from marker 198 for its ticks.
    LevelMessages messages;
    TowerWelcome fresh;
    fresh.open(world, true);
    fresh.arrived(device, messages, nullptr, world.layout(), sumner);
    REQUIRE(fresh.intro() == TowerWelcome::Intro::Crystal);
    REQUIRE(fresh.camera().has_value());
    s32 held = 0;
    while (fresh.hold(1)) {
        ++held;
    }
    CHECK(held == TowerWelcome::kCrystalTicks);
    CHECK(fresh.intro() == TowerWelcome::Intro::Done);
}

} // namespace
