#include <array>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

#include "TestSupport.h"
#include "game/app/ConsoleRoster.h"
#include "game/app/LevelDebugOverlay.h"
#include "game/camera/FreeCamera.h"
#include "game/characters/CharacterSave.h"
#include "game/config/GameConfig.h"
#include "game/console/ConsoleActionSink.h"
#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleForm.h"
#include "game/console/ConsoleListings.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleTranscript.h"
#include "game/console/DevConsole.h"
#include "game/console/RegisteredCommands.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/rollback/SharedConsoleWords.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/SceneSelection.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Words = std::vector<std::string>;

/** The words a Legends map's preview admits. */
constexpr std::array<std::string_view, 2> kPreviewWords{"help", "goto"};

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

/** What help says of each command after its name, as the application registers them. */
struct Summary {
    std::string_view name;
    std::string_view summary;
};
constexpr std::array<Summary, 18> kSummaries{{
    {"help", "lists the commands"},
    {"console_permissions", "0 or 1: host allows guests to use the console"},
    {"debug_info", "shows or hides the level debug overlay"},
    {"hidehud", "hides or shows every HUD element"},
    {"freecam", "lets the camera off the party to fly; you stand still"},
    {"list", "maps, items or inventory: what there is"},
    {"activate", "$SELECTED, switch#N, an id or a name: fires that switch as if stood on"},
    {"unlock", "a door or chest without a key, or character <code> for the select screen"},
    {"lock", "a door shut or a chest wanting a key again, or character <code> hidden again"},
    {"goto", "<map> [x y z], x y z or <legends map> [preview]: brings the party there"},
    {"unlock_tower", "opens every wing and portal of the tower"},
    {"get", "<item> [amount], exp <points> or level <level>: for you"},
    {"spawn", "stands enemies, great ones, generators, pickups or objects before you; spawn "
              "alone says how"},
    {"suicide", "fells everyone standing"},
    {"notarget", "the enemies stop seeing you, or see you again"},
    {"god", "nothing harms you, or everything does again"},
    {"noclip", "flies you through everything, or sets you down again"},
    {"party_count", "<1-4> or off: the party size the level sets itself up for"},
}};

/** A sink of the test's own: it keeps the lines it is given. */
class RecordingSink final : public ConsoleActionSink {
public:
    std::optional<std::string> submit(std::string line) override {
        lines.push_back(std::move(line));
        return std::nullopt;
    }

    std::vector<std::string> lines;
};

/** The application's roster over made-up game state with nothing in play, and an expansion of
 * the test's own: a Legends maps line for list maps and a goto form for one word. */
struct Roster {
    LevelDebugOverlay levelDebug;
    bool hudHidden = false;
    FreeCamera freeCamera;
    LevelCatalog levels;
    std::filesystem::path root = test::scratchDirectory("console-roster-unpacked");
    RecordingSink sink;
    SaveSlots saves;
    GameConfig config;
    std::optional<s32> partyCount;
    ConsoleListings listings;
    DevConsole console;

    Roster() {
        REQUIRE(saves.open(test::scratchDirectory("console-roster-saves"), 4));
        const ConsoleRosterAccess access{
            .online = {.permitGuests = [](bool /*allow*/) { return false; }},
            .view = {.levelDebug = levelDebug,
                     .hudHidden = hudHidden,
                     .freeCamera = freeCamera,
                     .view = [] { return std::optional<WorldCamera>{}; },
                     .levelDebugLines = [] { return Words{}; }},
            .listings = {.viewed = []() -> const LevelWorld* { return nullptr; },
                         .levels = levels,
                         .unpackedRoot = root,
                         .save = []() -> const CharacterSave* { return nullptr; }},
            .fixtures = {.scene = []() -> PlayScene* { return nullptr; },
                         .world = []() -> const LevelWorld* { return nullptr; }},
            .shared = {.sink = sink,
                       .levels = levels,
                       .legendsMapNames = [] { return Words{"castle1", "tower2"}; }},
            .inspect = {.selected = [] { return std::optional<SceneSelection>{}; },
                        .describe = [](const SceneSelection& /*selection*/) { return Words{}; }},
            .partyCount = {.count = partyCount, .scene = []() -> PlayScene* { return nullptr; }},
            .profile = {.saves = saves,
                        .config = config,
                        .scene = []() -> PlayScene* { return nullptr; }},
        };
        registerConsoleRoster(
            console.host(), console.commands(), listings, access,
            [](ConsoleHost& host, ConsoleListings& more) {
                more.add(std::make_unique<LambdaListing>("maps", std::nullopt,
                                                         [](ConsoleListing::Arguments /*rest*/) {
                                                             return Words{"n64: castle1 tower2"};
                                                         }));
                host.addForm("goto",
                             std::make_unique<LambdaForm>(
                                 ConsoleReach::OfflineOnly, "Legends maps are offline only.",
                                 [](ConsoleForm::Arguments arguments) {
                                     return arguments.size() == 1 && arguments[0] == "lab";
                                 },
                                 [](DevConsole& shown, ConsoleForm::Arguments /*words*/) {
                                     shown.print("walked");
                                 }));
            });
    }

    std::string say(std::string_view line, const ConsoleSession& session = {}) {
        console.setSession(session);
        return test::said(console, line);
    }
};

/** help's lines for `names`, each padded to the widest registered name. */
std::string helpOf(std::initializer_list<std::string_view> names) {
    std::string help;
    for (const std::string_view name : names) {
        for (const Summary& entry : kSummaries) {
            if (entry.name == name) {
                help += help.empty() ? "" : "\n";
                help += test::shownAs(std::format("{:<19}  {}", entry.name, entry.summary));
            }
        }
    }
    return help;
}
} // namespace

TEST_CASE("the roster registers today's commands at their reaches, in help's order",
          "[dev-console][online-console]") {
    Roster fixture;
    const auto commands = fixture.console.commands().commands();
    REQUIRE(commands.size() == test::kRegisteredCommands.size());
    for (usize i = 0; i < commands.size(); ++i) {
        CAPTURE(i);
        CHECK(commands[i]->name() == test::kRegisteredCommands[i].name);
        CHECK(commands[i]->reach() == test::kRegisteredCommands[i].reach);
    }
    // The shared commands are the wire's words, in its order, and a word the asking machine
    // keeps is a local form of its command.
    Words shared;
    for (const auto& command : commands) {
        if (command->reach() == ConsoleReach::Shared) {
            shared.emplace_back(command->name());
        }
    }
    Words wire;
    for (const SharedConsoleWord& word : kSharedConsoleWords) {
        wire.emplace_back(word.name);
        if (!word.localForm.empty()) {
            CAPTURE(word.name);
            const Words local{std::string(word.localForm)};
            const ConsoleForm* form = fixture.console.commands().claimant(word.name, local);
            REQUIRE(form != nullptr);
            CHECK(form->reach() == ConsoleReach::Local);
        }
    }
    CHECK(shared == wire);
}

TEST_CASE("help lists the commands in the order they are registered",
          "[dev-console][online-console]") {
    Roster fixture;
    CHECK(fixture.say("help") ==
          helpOf({"help", "debug_info", "hidehud", "freecam", "list", "activate", "unlock", "lock",
                  "goto", "unlock_tower", "get", "spawn", "suicide", "notarget", "god", "noclip",
                  "party_count"}));
    CHECK(fixture.say("help", {.seat = ConsoleSeat::Host}) ==
          helpOf({"help", "console_permissions", "debug_info", "hidehud", "freecam", "list", "goto",
                  "unlock_tower", "get", "spawn", "suicide", "notarget", "god", "noclip"}));
    CHECK(fixture.say("help", ConsolePolicy::session({.preview = kPreviewWords, .parked = true})) ==
          helpOf({"help", "activate", "unlock", "lock", "goto", "party_count"}));
}

TEST_CASE("names complete as they did before help's order changed",
          "[dev-console][online-console][autocomplete]") {
    Roster fixture;
    constexpr std::array<std::string_view, 20> kPrefixes{
        "a",  "d", "f",  "p",   "g", "go", "ge", "h",  "hi",     "l",
        "lo", "n", "no", "noc", "s", "su", "u",  "un", "unlock", "c"};
    struct Expected {
        ConsoleSession session;
        std::array<std::string_view, 20> rests;
    };
    const std::array<Expected, 4> rows{{
        {.session = {},
         .rests = {"ctivate", "ebug_info", "reecam", "arty_count", "oto",     "to",     "t",
                   "elp",     "dehud",     "ist",    "ck",         "otarget", "target", "lip",
                   "pawn",    "icide",     "nlock",  "lock",       "_tower",  ""}},
        {.session = {.seat = ConsoleSeat::Host},
         .rests = {"",      "ebug_info",   "reecam",     "",       "oto",
                   "to",    "t",           "elp",        "dehud",  "ist",
                   "",      "otarget",     "target",     "lip",    "pawn",
                   "icide", "nlock_tower", "lock_tower", "_tower", "onsole_permissions"}},
        {.session = {.seat = ConsoleSeat::Guest},
         .rests = {"",     "ebug_info", "reecam",      "",           "oto",     "to",     "t",
                   "elp",  "dehud",     "ist",         "",           "otarget", "target", "lip",
                   "pawn", "icide",     "nlock_tower", "lock_tower", "_tower",  ""}},
        {.session = ConsolePolicy::session({.preview = kPreviewWords, .parked = true}),
         .rests = {"ctivate", "", "", "arty_count", "oto", "to", "",      "elp",  "", "ock",
                   "ck",      "", "", "",           "",    "",   "nlock", "lock", "", ""}},
    }};
    for (const Expected& row : rows) {
        fixture.console.setSession(row.session);
        for (usize i = 0; i < kPrefixes.size(); ++i) {
            CAPTURE(static_cast<s32>(row.session.seat), kPrefixes[i]);
            CHECK(test::suggestionAfter(fixture.console, kPrefixes[i]) == row.rests[i]);
        }
    }
}

TEST_CASE("lock, unlock and goto complete their words as they did before",
          "[dev-console][autocomplete]") {
    Roster fixture;
    CHECK(test::suggestionAfter(fixture.console, "lock c") == "haracter");
    CHECK(test::suggestionAfter(fixture.console, "unlock c") == "haracter");
    CHECK(test::suggestionAfter(fixture.console, "lock character M") == "IN");
    // character in another case is not completed.
    CHECK(test::suggestionAfter(fixture.console, "lock Character M").empty());
    const ConsoleCommand* go = fixture.console.commands().find("goto");
    REQUIRE(go != nullptr);
    CHECK(go->completions(Words{"t"}) == Words{"tower", "back", "castle1", "tower2"});
    CHECK(go->completions(Words{"x", "p"}) == Words{"preview"});
    CHECK(go->completions(Words{"x", "p", "q"}) == Words{});
    CHECK(test::suggestionAfter(fixture.console, "goto b") == "ack");
    CHECK(test::suggestionAfter(fixture.console, "list c") == "ritters");
    CHECK(test::suggestionAfter(fixture.console, "list ch") == "aracters");
    CHECK(test::suggestionAfter(fixture.console, "party_count o") == "ff");
}

TEST_CASE("list's usage reads as it does today, and the expansions list after the game",
          "[dev-console][commands]") {
    Roster fixture;
    CHECK(fixture.say("list") == test::shownAs(kListUsage));
    CHECK(fixture.say("list nonsense") == test::shownAs(kListUsage));
    CHECK(fixture.listings.words() == Words{"maps", "items", "inventory", "enemies", "critters",
                                            "generators", "objects", "switches", "characters"});
    // With no level catalogued, list maps says only what the expansion adds.
    CHECK(fixture.say("list maps") == "n64: castle1 tower2");
    // The expansion's goto form answers its own words offline, and is refused online.
    CHECK(fixture.say("goto lab") == "walked");
    CHECK(fixture.say("goto lab", {.seat = ConsoleSeat::Guest}) ==
          "Legends maps are offline only.");
    CHECK(fixture.sink.lines.empty());
    CHECK(fixture.say("god").empty());
    CHECK(fixture.sink.lines == Words{"god"});
}

TEST_CASE("goto completes to the catalogue's levels after the tower", "[dev-console][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    Roster fixture;
    REQUIRE(fixture.levels.load(root));
    const ConsoleCommand* go = fixture.console.commands().find("goto");
    REQUIRE(go != nullptr);
    CHECK(go->completions(Words{"t"}) ==
          Words{"tower", "back", "T1", "T2", "T3", "A1", "A6", "A2",      "A3",    "A4", "A5", "B1",
                "B2",    "B3",   "B4", "B5", "B6", "C1", "C2", "C3",      "C4",    "C5", "D1", "D2",
                "D3",    "D4",   "D5", "E1", "E2", "F1", "F2", "G1",      "G2",    "G3", "G4", "G5",
                "H1",    "H2",   "H3", "H4", "I1", "I2", "I3", "I4",      "I5",    "J1", "J2", "J3",
                "J6",    "J4",   "J5", "K2", "K3", "K4", "K1", "K5",      "S1",    "S2", "S3", "S4",
                "S5",    "S6",   "S7", "S8", "S9", "L1", "L2", "castle1", "tower2"});
}
