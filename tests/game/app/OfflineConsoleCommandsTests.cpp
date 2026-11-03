#include <algorithm>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/OfflineConsoleCommands.h"
#include "game/characters/ClassData.h"
#include "game/console/DevConsole.h"
#include "game/console/FixtureRequest.h"
#include "game/console/LevelListingLines.h"
#include "game/context/GameContext.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr s32 kSeats = 2;
/** What the made-up Legends ports answer: a line for list maps, and two names. */
constexpr std::string_view kLegendsLine = "n64: castle1 tower2";

/** What activate answers words it cannot read. */
constexpr std::string_view kActivateUsage =
    "activate $SELECTED     fires the switch selected, as if you stood on it\n"
    "activate switch#N      the same, by the token list switches gives\n"
    "activate <id or name>  the same, by a switch's id, its name or its target's\n"
    "The camera follows what it opens, as it does for a player.";

/** What list answers words it cannot read. */
constexpr std::string_view kListUsage =
    "list maps              every level, by realm\n"
    "list items [map]       what get knows, then a level's pickups\n"
    "list inventory         what your character is and carries\n"
    "list enemies [map]     the swarm's kinds, then a level's roster and generators\n"
    "list critters [map]    the great ones, then those a level places\n"
    "list generators [map]  a level's generators\n"
    "list objects [map]     a level's objects\n"
    "list switches [map]    a level's switches, for activate\n"
    "list characters        every playable character, locked or hidden too";

using Words = std::vector<std::string>;

/** The words of `line`, split on spaces as the console splits them. */
Words wordsOf(std::string_view line) {
    Words words;
    usize at = 0;
    while (at < line.size()) {
        const usize start = line.find_first_not_of(' ', at);
        if (start == std::string_view::npos) {
            break;
        }
        const usize end = std::min(line.find(' ', start), line.size());
        words.emplace_back(line.substr(start, end - start));
        at = end;
    }
    return words;
}

/** What a fresh console shows once `run` has printed to it, its lines joined by breaks. */
template <typename Run> std::string shown(Run&& run) {
    DevConsole console;
    std::forward<Run>(run)(console);
    std::string text;
    for (const std::string& line : console.output()) {
        text += text.empty() ? "" : "\n";
        text += line;
    }
    return text;
}

/** What a console shows of `lines` printed one after another. */
std::string printed(std::span<const std::string> listing) {
    return shown([&](DevConsole& console) {
        for (const std::string& line : listing) {
            console.print(line);
        }
    });
}

/** `parts` one after another, as one listing. */
Words lines(std::initializer_list<Words> parts) {
    Words all;
    for (const Words& part : parts) {
        all.insert(all.end(), part.begin(), part.end());
    }
    return all;
}

/** The offline console commands over a scene and world of the test's own, their ports reading
 * the application state held here; the Legends maps are made up. */
struct Commands {
    std::filesystem::path root;
    test::FakeRenderDevice device;
    GameConfig config;
    LevelCatalog catalog;
    StringTable strings;
    f32 stopTimeTotal = 0;
    SaveSlots saves;
    LevelWorld world;
    PlayScene scene;
    GameContext context;
    std::optional<s32> partyCount;
    OfflineConsoleCommands commands{ports()};

    /** Nothing in play: the scene closed, the world never loaded, nothing unpacked. */
    Commands() : root(test::scratchDirectory("offline-console-unpacked")) {
        REQUIRE(saves.open(test::scratchDirectory("offline-console-saves"), 4));
    }

    /** `levelName` open for two seats (the tower for "L1"). */
    explicit Commands(std::string_view levelName)
        : root(test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2")
                   .parent_path()
                   .parent_path()
                   .parent_path()) {
        test::assetOrSkip(std::format("LEVELS/LEVEL{}/WORLDS.PS2", levelName));
        REQUIRE(saves.open(test::scratchDirectory("offline-console-saves"), 4));
        config.timing.tickRate = 60;
        REQUIRE(catalog.load(root));
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        const auto level =
            levelName == "L1" ? std::optional{LevelRef::tower()} : catalog.byName(levelName);
        REQUIRE(level);
        REQUIRE(world.load(device, root, *level));
        context.config = &config;
        context.levels = &catalog;
        context.strings = &strings;
        context.unpackedRoot = root;
        context.stopTimeTotal = &stopTimeTotal;
        std::vector<PartyMember> party;
        for (s32 seat = 0; seat < kSeats; ++seat) {
            CharacterSave save;
            save.name = "TESTER";
            save.character = seat;
            save.color = seat;
            save.progress().health = 4000;
            party.push_back({seat, save, static_cast<usize>(seat), false, 100});
        }
        PlayOptions options;
        options.welcome = false;
        REQUIRE(scene.open(device, context, world, party, options));
    }

    OfflineConsoleCommands::Ports ports() {
        return {
            .playScene = [this] { return &scene; },
            .consoleScene = [this]() -> const PlayScene* {
                return scene.isOpen() ? &scene : nullptr;
            },
            .consoleWorld = [this]() -> const LevelWorld* {
                return scene.isOpen() ? &world : nullptr;
            },
            .consolePlayer = [this] { return player(); },
            .levels = catalog,
            .unpackedRoot = root,
            .config = config,
            .saves = saves,
            .partyCount = partyCount,
            .legendsMapLines = [] { return Words{std::string{kLegendsLine}}; },
            .legendsMapNames = [] { return Words{"castle1", "tower2"}; },
        };
    }

    s32 player() const {
        for (s32 seat = 0; seat < PlayScene::kPlayerCount; ++seat) {
            if (scene.actor(seat) != nullptr) {
                return seat;
            }
        }
        return -1;
    }

    /** What `handler` shows for the words of `line`. */
    std::string say(void (OfflineConsoleCommands::*handler)(DevConsole&,
                                                            OfflineConsoleCommands::Arguments),
                    std::string_view line) {
        const Words words = wordsOf(line);
        return shown([&](DevConsole& console) { (commands.*handler)(console, words); });
    }

    CharacterSave& save(s32 seat) {
        CharacterSave* live = scene.liveSave(seat);
        REQUIRE(live != nullptr);
        return *live;
    }
};
} // namespace

TEST_CASE("with nothing in play each offline command answers in its own words", "[dev-console]") {
    Commands fixture;
    using C = OfflineConsoleCommands;
    CHECK(fixture.say(&C::activate, "") == kActivateUsage);
    CHECK(fixture.say(&C::activate, "switch#0") == "Nothing is in play.");
    CHECK(
        fixture.say(&C::lock, "") ==
        "lock $SELECTED|gate#N|chest#N|<name>  a door shut again, or a chest wanting a key again\n"
        "lock character <code>                 a character hidden again; the starting eight stay\n"
        "list characters names the codes; list switches and get info name the rest.");
    CHECK(fixture.say(&C::lock, "chest#0") == "Nothing is in play.");
    CHECK(fixture.say(&C::unlock, "gate#0") == "Nothing is in play.");
    CHECK(fixture.say(&C::unlock, "character XYZ") ==
          "No character is coded XYZ; list characters names them");
    CHECK(fixture.say(&C::unlock, "character WAR") == "WAR is always there to choose");
    const std::string code{classCode(kStartingClassCount)};
    CHECK(fixture.say(&C::unlock, "character " + code) ==
          "MIN unlocked: the select screen offers it");
    CHECK((fixture.saves.classUnlocks() & 1U) != 0);
    CHECK(fixture.say(&C::lock, "character " + code) ==
          "MIN locked: the select screen hides it again");
    CHECK((fixture.saves.classUnlocks() & 1U) == 0);

    CHECK(fixture.say(&C::partyCount, "") == "The level is set up for the players present");
    CHECK(fixture.say(&C::partyCount, "3") == "The level is set up for a party of 3");
    CHECK(fixture.partyCount == 3);
    CHECK(fixture.say(&C::partyCount, "5") ==
          "party_count <1-4>  sets the level up for a party that size\n"
          "party_count off    follows the players present again\n"
          "party_count        says which it is");
    CHECK(fixture.partyCount == 3);
    CHECK(fixture.say(&C::partyCount, "off") == "The level is set up for the players present");
    CHECK_FALSE(fixture.partyCount.has_value());

    CHECK(fixture.say(&C::list, "") == kListUsage);
    CHECK(fixture.say(&C::list, "nonsense") == kListUsage);
    CHECK(fixture.say(&C::list, "maps") == "n64: castle1 tower2");
    CHECK(fixture.say(&C::list, "inventory") == "Nobody is in.");
    CHECK(fixture.say(&C::list, "switches") == "Nothing is in play.");
    CHECK(fixture.say(&C::list, "objects") == "Nothing is in play.");
    CHECK(fixture.say(&C::list, "generators") == "Nothing is in play.");
    CHECK(fixture.say(&C::list, "items G1") == "No map named G1; list maps names them");
    CHECK(fixture.say(&C::list, "objects TOWER") == "L1 is not unpacked");
    CHECK(fixture.say(&C::list, "items") == printed(lines({itemLines(), {"Nothing is in play."}})));
    CHECK(fixture.say(&C::list, "enemies") ==
          "Enemies (spawn <enemy> [tier 1-3] [count]): SCO, TRO, DEM, RAT, GRU, KNI, SNA, SOR, "
          "MUM, SPI, LIZ, TRE, MAG, ZOM, PLA, WOL,\n"
          "ICE, WRM, DOG, SKE, GHO, ACI, HAN, IMP, WAR, SKY, WIND, GRM, DEATH, IT\n"
          "Nothing is in play.");
    CHECK(fixture.say(&C::list, "critters") ==
          "Critters (spawn <critter>): golem, general, gargoyle [eagl|lion|serp]\n"
          "Nothing is in play.");
    CHECK(fixture.say(&C::list, "characters") ==
          printed(characterLines(fixture.saves.classUnlocks())));

    CHECK(fixture.commands.activateWords(Words{"s"}) == Words{});
    CHECK(fixture.commands.lockWords(Words{"c"}) == Words{"character"});
    CHECK(fixture.commands.lockWords(Words{"character", "M"}) ==
          Words{"MIN", "FAL", "JAC", "TIG", "OGR", "UNI", "MED", "HYE", "SUM"});
    CHECK(fixture.commands.lockWords(Words{"CHARACTER", "M"}) ==
          Words{"MIN", "FAL", "JAC", "TIG", "OGR", "UNI", "MED", "HYE", "SUM"});
    CHECK(fixture.commands.lockWords(Words{"Character", "M"}) == Words{});
    CHECK(fixture.commands.lockWords(Words{"character", "M", "x"}) == Words{});
    CHECK(fixture.commands.gotoWords(Words{"t"}) == Words{"tower", "back", "castle1", "tower2"});
    CHECK(fixture.commands.gotoWords(Words{"x", "p"}) == Words{"preview"});
    CHECK(fixture.commands.gotoWords(Words{"x", "p", "q"}) == Words{});
}

TEST_CASE("every offline command answers in a level in its own words", "[dev-console][assets]") {
    auto fixture = std::make_unique<Commands>("G2");
    auto& scene = fixture->scene;
    using C = OfflineConsoleCommands;
    PlayScene::LocalSnapshot opened;
    REQUIRE(scene.saveLocal(opened));
    const auto reopen = [&] { REQUIRE(scene.restoreLocal(opened)); };

    // party_count reaches the open scene.
    CHECK(fixture->say(&C::partyCount, "3") == "The level is set up for a party of 3");
    CHECK(scene.partyCountOverride() == 3);
    CHECK(fixture->say(&C::partyCount, "off") == "The level is set up for the players present");
    CHECK_FALSE(scene.partyCountOverride().has_value());

    // The level's switches, doors and chests, by the words the console offers for them.
    CHECK(fixture->commands.activateWords(Words{"s"}) ==
          Words{"switch#0",  "BRIDGEPAD", "switch#1",  "BRIDGEPAD", "switch#2",  "BRIDGEPAD",
                "switch#3",  "BRIDGEPAD", "switch#4",  "BRIDGEPAD", "switch#5",  "BRIDGEPAD",
                "switch#6",  "BRIDGEPAD", "switch#7",  "BRIDGEPAD", "switch#8",  "BRIDGEPAD",
                "switch#9",  "BRIDGEPAD", "switch#10", "BRIDGEPAD", "switch#11", "BRIDGEPAD",
                "switch#12", "BRIDGEPAD", "switch#13", "BRIDGEPAD", "switch#14", "BRIDGEPAD",
                "switch#15", "BRIDGEPAD", "switch#16", "BRIDGEPAD", "switch#17", "TRIGGER3",
                "switch#18", "TRIGGER3",  "switch#19", "TRIGGER3",  "switch#20", "TRIGGER3",
                "switch#21", "TRIGGER3",  "switch#22", "TRIGGER3",  "switch#23", "TRIGGER3",
                "switch#24", "TRIGGER2",  "switch#25", "TRIGGER2",  "switch#26", "TRIGGER2",
                "switch#27", "TRIGGER2",  "switch#28", "TRIGGER2",  "switch#29", "TRIGGER2",
                "switch#30", "TRIGGERS"});
    CHECK(fixture->commands.lockWords(Words{"c"}) ==
          Words{"character", "gate#0",   "GATEB",    "gate#1",   "GATEB",    "gate#2",   "GATED",
                "chest#0",   "CHEST",    "chest#1",  "CHEST",    "chest#2",  "CHEST",    "chest#4",
                "CHEST",     "chest#6",  "CHEST",    "chest#7",  "CHEST",    "chest#11", "CHEST",
                "chest#13",  "CHEST",    "chest#14", "CHEST",    "chest#16", "CHEST",    "chest#17",
                "CHEST",     "chest#18", "CHEST",    "chest#20", "CHESTG1",  "chest#21", "CHESTG3",
                "chest#22",  "CHESTG2",  "chest#24", "CHESTG4",  "chest#25", "CHESTG0",  "chest#26",
                "CHESTEXP",  "chest#27", "CHESTEXP"});
    CHECK(fixture->commands.lockWords(Words{"character", "M"}) ==
          Words{"MIN", "FAL", "JAC", "TIG", "OGR", "UNI", "MED", "HYE", "SUM"});
    CHECK(fixture->commands.lockWords(Words{"Character", "M"}) == Words{});
    CHECK(fixture->say(&C::activate, "") == kActivateUsage);
    CHECK(fixture->say(&C::activate, "NOPE") == "NOPE names no switch here; list switches names "
                                                "them, or click a pad and activate $SELECTED");
    CHECK(
        fixture->say(&C::activate, "21") ==
        "21 names no switch here; list switches names them, or click a pad and activate $SELECTED");
    CHECK(fixture->say(&C::activate, "BRIDGEPAD") ==
          "BRIDGEPAD names 17 switches: switch#0, switch#1, switch#2, switch#3, switch#4, "
          "switch#5, switch#6, switch#7, switch#8,\n"
          "switch#9, switch#10, switch#11, switch#12, switch#13, switch#14, switch#15, switch#16; "
          "say which by its token");
    CHECK(fixture->say(&C::activate, "G2ELEV8") ==
          "G2ELEV8 names 3 switches: switch#11, switch#19, switch#20; say which by its token");
    CHECK(fixture->say(&C::activate, "switch#0") ==
          "Pressed switch#0 BRIDGEPAD: G2ELEV7 answers, the camera following");
    CHECK(fixture->say(&C::activate, "switch#0") ==
          "Pressed switch#0 BRIDGEPAD: G2ELEV7 answers, the camera following");
    CHECK(fixture->say(&C::activate, "switch#1") == "Pressed switch#1 BRIDGEPAD");
    CHECK(fixture->say(&C::activate, "switch#23") ==
          "switch#23 TRIGGER3 is not for this party, or stands open for good");
    CHECK(fixture->say(&C::activate, "g2elev7") ==
          "Pressed switch#0 BRIDGEPAD: G2ELEV7 answers, the camera following");
    CHECK(fixture->say(&C::activate, "triggers") ==
          "Pressed switch#30 TRIGGERS: G2BAR_WALL answers, the camera following");
    reopen();
    CHECK(fixture->say(&C::unlock, "NOPE") ==
          "NOPE names no door or chest here; gate#N, chest#N, a name or $SELECTED after a click "
          "does");
    CHECK(fixture->say(&C::unlock, "GATEB") ==
          "GATEB names 2 things: gate#0, gate#1; say which by its token");
    CHECK(fixture->say(&C::unlock, "gate#0") == "gate#0 GATEB unlocked: it swings open");
    CHECK(fixture->say(&C::unlock, "gate#0") == "gate#0 GATEB is open already");
    CHECK(fixture->say(&C::lock, "gate#0") == "gate#0 GATEB locked: shut until a key is spent");
    CHECK(fixture->say(&C::lock, "gate#0") == "gate#0 GATEB is shut already");
    CHECK(fixture->say(&C::unlock, "gated") == "gate#2 GATED unlocked: it swings open");
    CHECK(fixture->say(&C::unlock, "chest#0") == "chest#0 CHEST unlocked: it opens without a key");
    CHECK(fixture->say(&C::lock, "chest#0") == "chest#0 CHEST locked: a key opens it");
    CHECK(fixture->say(&C::lock, "chest#0") == "chest#0 CHEST locked: a key opens it");
    CHECK(fixture->say(&C::unlock, "CHESTG1") ==
          "chest#20 CHESTG1 unlocked: it opens without a key");
    CHECK(fixture->say(&C::lock, "CHEST") ==
          "CHEST names 12 things: chest#0, chest#1, chest#2, chest#4, chest#6, chest#7, chest#11, "
          "chest#13, chest#14, chest#16, chest#17,\n"
          "chest#18; say which by its token");
    reopen();
    const std::string code{classCode(kStartingClassCount)};
    fixture->save(0).classUnlock = 1U;
    CHECK(fixture->say(&C::lock, "character " + code) ==
          "MIN locked: the select screen hides it again");
    CHECK(fixture->save(0).classUnlock == 0);

    // The listings.
    const auto g1 = fixture->catalog.byName("G1");
    REQUIRE(g1);
    WorldLayout fieldsLayout;
    REQUIRE(fieldsLayout.load(fixture->root / g1->directory));
    CHECK(
        fixture->say(&C::list, "maps") ==
        "T test (0): T1 TEST1, T2 TEST2, T3 TEST3\n"
        "A castle (1): A1 Courtyard, A6 Dungeon, A2 Barracks, A3 Armory, A4 Treasury, A5 Chimera\n"
        "B mount (2): B1 Valley, B2 Peak, B3 Cliff, B4 Cave, B5 Cavern, B6 Dragon\n"
        "C desert (3): C1 Ruins, C2 Temple, C3 Pyramid, C4 Tombs, C5 Genie\n"
        "D forest (4): D1 Swamp, D2 Roots, D3 Tree, D4 Branches, D5 Spider\n"
        "E temple (5): E1 Temple, E2 Altar\n"
        "F hell (6): F1 Gates, F2 Throne\n"
        "G town (7): G1 Fields, G2 Town, G3 Cemetary, G4 Mausoleum, G5 Lich\n"
        "H battle (8): H1 Trenches, H2 Towers, H3 Fortress, H4 Citadel\n"
        "I ice (9): I1 Docks, I2 Camp, I3 Mine, I4 Fissure, I5 Yeti\n"
        "J dream (10): J1 Carnival, J2 Grounds, J3 House, J6 Nightmare, J4 Illusion, J5 Wraith\n"
        "K sky (11): K2 Spire, K3 Docks, K4 Shipyard, K1 Mothership, K5 Fiend\n"
        "S secret (12): S1 Lab, S2 Psychedelic, S3 Space, S4 Gauntlet, S5 Trenches, S6 Docks, S7 "
        "Henhouse, S8 Crops, S9 Cloud9\n"
        "L tower (13): L1 Tower, L2 Tower\n"
        "n64: castle1 tower2");
    CHECK(fixture->say(&C::list, "inventory") ==
          "TESTER the WAR at level 1 (0 experience), health 4000/500, 0 gold\n"
          "Keys 0/9, potions 0/9\n"
          "No powerups\n"
          "Runestones 0/13, boss shards 0, legend items 0, gargoyle pieces 0/12 0/20 0/28\n"
          "Crystals: none");
    CHECK(fixture->say(&C::list, "enemies") ==
          "Enemies (spawn <enemy> [tier 1-3] [count]): SCO, TRO, DEM, RAT, GRU, KNI, SNA, SOR, "
          "MUM, SPI, LIZ, TRE, MAG, ZOM, PLA, WOL,\n"
          "ICE, WRM, DOG, SKE, GHO, ACI, HAN, IMP, WAR, SKY, WIND, GRM, DEATH, IT\n"
          "In G2:\n"
          "Roster: medium ZOM, small MAG, GOLEM, GENERAL, GAR\n"
          "Generators: GRU (tier 3); KNI x72 (tier 1, 2, 3); RAT x41 (tier 1, 2)\n"
          "Placed enemies: DEATH x3, GRU, KNI x45");
    CHECK(fixture->say(&C::list, "enemies G1") ==
          "In G1:\n"
          "Roster: medium ZOM, small MAG, GOLEM, GENERAL, GAR\n"
          "Generators: GRU x102 (tier 1, 2, 3); RAT x15 (tier 1)\n"
          "Placed enemies: DEATH x2, GHO, GRU x45");
    CHECK(fixture->say(&C::list, "critters") ==
          "Critters (spawn <critter>): golem, general, gargoyle [eagl|lion|serp]\n"
          "In G2:\n"
          "Critters placed: GAR, GENERAL x4, GOLEM x2");
    CHECK(fixture->say(&C::list, "generators") ==
          "In G2:\n"
          "Generators: GRU (tier 3); KNI x72 (tier 1, 2, 3); RAT x41 (tier 1, 2)\n"
          "spawn generator <enemy> [tier] stands another");
    CHECK(fixture->say(&C::list, "items Z9") == "No map named Z9; list maps names them");
    // The longest listings are the listing lines themselves, under the level's name.
    const WorldLayout& layout = fixture->world.layout();
    CHECK(fixture->say(&C::list, "switches") ==
          printed(lines({{"In G2:"}, switchLines(fixture->commands.consoleSwitches())})));
    CHECK(fixture->say(&C::list, "switches G1") ==
          printed(lines({{"In G1:"}, switchLines(switchesOf(fieldsLayout))})));
    CHECK(fixture->say(&C::list, "items G1") ==
          printed(lines({{"In G1:"}, pickupLines(fieldsLayout)})));
    CHECK(fixture->say(&C::list, "characters") ==
          printed(characterLines(fixture->saves.classUnlocks())));
    CHECK(fixture->say(&C::list, "items") ==
          printed(lines({itemLines(), {"In G2:"}, pickupLines(layout)})));
    CHECK(fixture->say(&C::list, "objects") == printed(lines({{"In G2:"}, objectLines(layout)})));
    CHECK(fixture->commands.gotoWords(Words{"t"}) ==
          Words{"tower", "back", "T1", "T2", "T3", "A1", "A6", "A2",      "A3",    "A4", "A5", "B1",
                "B2",    "B3",   "B4", "B5", "B6", "C1", "C2", "C3",      "C4",    "C5", "D1", "D2",
                "D3",    "D4",   "D5", "E1", "E2", "F1", "F2", "G1",      "G2",    "G3", "G4", "G5",
                "H1",    "H2",   "H3", "H4", "I1", "I2", "I3", "I4",      "I5",    "J1", "J2", "J3",
                "J6",    "J4",   "J5", "K2", "K3", "K4", "K1", "K5",      "S1",    "S2", "S3", "S4",
                "S5",    "S6",   "S7", "S8", "S9", "L1", "L2", "castle1", "tower2"});
}
