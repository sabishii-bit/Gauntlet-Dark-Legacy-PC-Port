#include <array>
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

#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

#include "TestSupport.h"
#include "expansions/shared/app/LegendsConsoleWords.h"
#include "expansions/shared/characters/LegendsProgress.h"
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

/** The roster's commands after the console's help in a second order: party_count after goto,
 * hidehud and freecam after god. Each group of names sharing a first letter keeps the roster's
 * order. */
constexpr std::array<std::string_view, 17> kSecondOrder{"console_permissions",
                                                        "debug_info",
                                                        "list",
                                                        "activate",
                                                        "unlock",
                                                        "lock",
                                                        "goto",
                                                        "party_count",
                                                        "unlock_tower",
                                                        "get",
                                                        "spawn",
                                                        "suicide",
                                                        "notarget",
                                                        "god",
                                                        "hidehud",
                                                        "freecam",
                                                        "noclip"};

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

/** What help says of goto, and its usage, with no Legends map installed or visited. */
constexpr std::string_view kBaseGotoSummary = "<map> [x y z] or x y z: brings the party there";
constexpr std::string_view kBaseGotoUsage =
    "goto <map> [x y z]  travels to a level, standing at a point when given\n"
    "goto x y z          moves the party to that point here";

/** A sink of the test's own: it keeps the lines it is given. */
class RecordingSink final : public ConsoleActionSink {
public:
    std::optional<std::string> submit(std::string line) override {
        lines.push_back(std::move(line));
        return std::nullopt;
    }

    std::vector<std::string> lines;
};

/** Whether the Legends maps stand installed under the roster's console. */
enum class Maps : u8 { Absent, Installed };

/** The application's roster over made-up game state with nothing in play, and the Legends
 * expansion's words over made-up maps: castle1 and tower2 on the N64 when they are installed, and
 * the folder of lab whether they are or not. */
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
    bool legendsInstalled;

    explicit Roster(Maps maps = Maps::Installed) : legendsInstalled(maps == Maps::Installed) {
        REQUIRE(saves.open(test::scratchDirectory("console-roster-saves"), 4));
        LegendsConsoleAccess legends{
            .visiting = [] { return false; },
            .installed = [this](bool /*rescan*/) { return legendsInstalled; },
            .mapNames =
                [this](LegendsPlatform platform) {
                    return legendsInstalled && platform == LegendsPlatform::N64
                               ? Words{"castle1", "tower2"}
                               : Words{};
                },
            .find = [](std::string_view name, std::optional<LegendsPlatform> /*platform*/)
                -> std::optional<std::pair<LegendsPlatform, std::filesystem::path>> {
                if (name != "lab") {
                    return std::nullopt;
                }
                return std::pair{LegendsPlatform::N64, std::filesystem::path{"maps/lab"}};
            },
            .open = [](DevConsole& shown, LegendsPlatform /*platform*/,
                       const std::filesystem::path& /*directory*/,
                       bool walk) { shown.print(walk ? "walked" : "previewed"); },
            .close = [](DevConsole& shown) { shown.print("came back"); },
            .walkingNative = [] { return false; },
            .unlockTower = [](DevConsole& /*shown*/) {}};
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
            .shared = {.sink = sink, .levels = levels},
            .inspect = {.selected = [] { return std::optional<SceneSelection>{}; },
                        .describe = [](const SceneSelection& /*selection*/) { return Words{}; }},
            .partyCount = {.count = partyCount, .scene = []() -> PlayScene* { return nullptr; }},
            .profile = {.saves = saves,
                        .config = config,
                        .scene = []() -> PlayScene* { return nullptr; }},
        };
        registerConsoleRoster(console.host(), console.commands(), listings, access,
                              [&legends](ConsoleHost& host, ConsoleListings& more) {
                                  registerLegendsConsoleWords(host, more, std::move(legends));
                              });
    }

    std::string say(std::string_view line, const ConsoleSession& session = {}) {
        console.setSession(session);
        return test::said(console, line);
    }
};

/** A command of a name and a reach alone, which does nothing. */
class StandIn final : public ConsoleCommand {
public:
    StandIn(std::string_view name, ConsoleReach reach) : m_name(name), m_reach(reach) {}
    std::string_view name() const override { return m_name; }
    std::string summary() const override { return "stands in"; }
    ConsoleReach reach() const override { return m_reach; }
    void run(DevConsole& /*console*/, Arguments /*arguments*/) override {}

private:
    std::string_view m_name;
    ConsoleReach m_reach;
};

/** Registers on `console` stand-ins for the roster's commands in `order`, each at the reach the
 * application registers it with. */
void registerStandIns(DevConsole& console, std::span<const std::string_view> order) {
    for (const std::string_view name : order) {
        for (const test::RegisteredCommand& registered : test::kRegisteredCommands) {
            if (registered.name == name) {
                console.addCommand(std::make_unique<StandIn>(registered.name, registered.reach));
            }
        }
    }
}

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

TEST_CASE("the roster registers every command at its reach, in help's order",
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
    // A Legends map's preview runs its own words alone.
    CHECK(fixture.say("help",
                      ConsolePolicy::session({.preview = kLegendsPreviewWords, .parked = true})) ==
          helpOf({"help", "goto"}));
}

TEST_CASE("a name prefix completes to the first admitted name it begins, whatever help's order",
          "[dev-console][online-console][autocomplete]") {
    Roster fixture;
    // The same names at the same reaches in another order that keeps each group's order.
    DevConsole second;
    registerStandIns(second, kSecondOrder);
    REQUIRE(second.commands().size() == test::kRegisteredCommands.size());
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
        {.session = ConsolePolicy::session({.preview = kLegendsPreviewWords, .parked = true}),
         .rests = {"", "", "", "", "oto", "to", "", "elp", "", "",
                   "", "", "", "", "",    "",   "", "",    "", ""}},
    }};
    for (const Expected& row : rows) {
        fixture.console.setSession(row.session);
        second.setSession(row.session);
        for (usize i = 0; i < kPrefixes.size(); ++i) {
            CAPTURE(static_cast<s32>(row.session.seat), kPrefixes[i]);
            CHECK(test::suggestionAfter(fixture.console, kPrefixes[i]) == row.rests[i]);
            CHECK(test::suggestionAfter(second, kPrefixes[i]) == row.rests[i]);
        }
    }
}

TEST_CASE("lock, unlock, goto, list and party_count complete their words",
          "[dev-console][autocomplete]") {
    Roster fixture;
    CHECK(test::suggestionAfter(fixture.console, "lock c") == "haracter");
    CHECK(test::suggestionAfter(fixture.console, "unlock c") == "haracter");
    CHECK(test::suggestionAfter(fixture.console, "lock character M") == "IN");
    // character in another case is not completed.
    CHECK(test::suggestionAfter(fixture.console, "lock Character M").empty());
    const ConsoleRegistry& commands = fixture.console.commands();
    const ConsoleCommand* go = commands.find("goto");
    REQUIRE(go != nullptr);
    CHECK(commands.completions(*go, Words{"t"}) == Words{"tower", "back", "castle1", "tower2"});
    CHECK(commands.completions(*go, Words{"x", "p"}) == Words{"preview"});
    CHECK(commands.completions(*go, Words{"x", "p", "q"}) == Words{});
    CHECK(test::suggestionAfter(fixture.console, "goto b") == "ack");
    CHECK(test::suggestionAfter(fixture.console, "list c") == "ritters");
    CHECK(test::suggestionAfter(fixture.console, "list ch") == "aracters");
    CHECK(test::suggestionAfter(fixture.console, "party_count o") == "ff");
}

TEST_CASE(
    "list answers words it cannot read with its usage, and the expansions list after the game",
    "[dev-console][commands]") {
    Roster fixture;
    CHECK(fixture.say("list") == test::shownAs(kListUsage));
    CHECK(fixture.say("list nonsense") == test::shownAs(kListUsage));
    CHECK(fixture.listings.words() == Words{"maps", "items", "inventory", "enemies", "critters",
                                            "generators", "objects", "switches", "characters"});
    // With no level catalogued, list maps says only what the expansion adds.
    CHECK(fixture.say("list maps") == "N64 Legends maps (goto <name>): castle1, tower2");
    // The expansion's goto form answers its own words offline, and is refused online.
    CHECK(fixture.say("goto lab") == "walked");
    CHECK(fixture.say("goto lab", {.seat = ConsoleSeat::Guest}) ==
          "Legends maps are offline only.");
    CHECK(fixture.sink.lines.empty());
    CHECK(fixture.say("god").empty());
    CHECK(fixture.sink.lines == Words{"god"});
}

TEST_CASE("a character's code after lock or unlock is the profile's form", "[dev-console]") {
    Roster fixture;
    CHECK(fixture.say("unlock character XYZ") ==
          "No character is coded XYZ; list characters names them");
    CHECK(fixture.say("lock CHARACTER WAR") == "WAR is always there to choose");
}

TEST_CASE("without a Legends map installed, the console names none of their words",
          "[dev-console][online-console][legends]") {
    Roster fixture{Maps::Absent};
    const std::string help = fixture.say("help");
    CHECK(help.contains(test::shownAs(std::format("{:<19}  {}", "goto", kBaseGotoSummary))));
    CHECK_FALSE(help.contains("legends"));
    CHECK(fixture.say("goto") == test::shownAs(kBaseGotoUsage));
    CHECK(fixture.say("goto 1 2", {.seat = ConsoleSeat::Guest}) == test::shownAs(kBaseGotoUsage));
    const ConsoleRegistry& commands = fixture.console.commands();
    const ConsoleCommand* go = commands.find("goto");
    REQUIRE(go != nullptr);
    CHECK(commands.completions(*go, Words{"t"}) == Words{"tower"});
    CHECK(commands.completions(*go, Words{"x", "p"}).empty());
    CHECK(test::suggestionAfter(fixture.console, "goto b").empty());
    CHECK(fixture.say("list maps").empty());
    // The words are still the expansion's, and answered as they were.
    CHECK(fixture.say("goto lab") == "walked");
    CHECK(fixture.say("goto lab preview") == "previewed");
    CHECK(fixture.say("goto back") == "came back");
    CHECK(fixture.say("goto castle") ==
          "No level or Legends map named castle; list maps names them");
    CHECK(fixture.say("goto castle", {.seat = ConsoleSeat::Guest}) ==
          "Legends maps are offline only.");
    CHECK(fixture.sink.lines.empty());
}

TEST_CASE("goto completes to the catalogue's levels after the tower", "[dev-console][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    Roster fixture;
    REQUIRE(fixture.levels.load(root));
    const ConsoleCommand* go = fixture.console.commands().find("goto");
    REQUIRE(go != nullptr);
    CHECK(fixture.console.commands().completions(*go, Words{"t"}) ==
          Words{"tower", "back", "T1", "T2", "T3", "A1", "A6", "A2",      "A3",    "A4", "A5", "B1",
                "B2",    "B3",   "B4", "B5", "B6", "C1", "C2", "C3",      "C4",    "C5", "D1", "D2",
                "D3",    "D4",   "D5", "E1", "E2", "F1", "F2", "G1",      "G2",    "G3", "G4", "G5",
                "H1",    "H2",   "H3", "H4", "I1", "I2", "I3", "I4",      "I5",    "J1", "J2", "J3",
                "J6",    "J4",   "J5", "K2", "K3", "K4", "K1", "K5",      "S1",    "S2", "S3", "S4",
                "S5",    "S6",   "S7", "S8", "S9", "L1", "L2", "castle1", "tower2"});
}
