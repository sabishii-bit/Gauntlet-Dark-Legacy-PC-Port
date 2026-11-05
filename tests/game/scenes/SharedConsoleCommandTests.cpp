#include <array>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/console/ConsoleActionSink.h"
#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleForm.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleWords.h"
#include "game/console/DevConsole.h"
#include "game/console/GiftRequest.h"
#include "game/console/GotoRequest.h"
#include "game/level/LevelCatalog.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/SharedConsoleCommand.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr std::string_view kLegendsOffline = "Legends maps are offline only.";
constexpr std::string_view kRequested = "Command requested for the shared game.";

// What get, goto and spawn answer words they cannot read, word for word.
constexpr std::string_view kGetUsage =
    "get <item> [amount]   keys, potions, gold, health or a powerup\n"
    "get exp <points>      experience to add\n"
    "get level <level>     the level to be, 1 to 99\n"
    "get info [$SELECTED]  what the thing clicked in the view is\n"
    "list items names the items.";
/** goto's usage with no form listing the Legends maps: its own forms alone. */
constexpr std::string_view kGotoUsage =
    "goto <map> [x y z]  travels to a level, standing at a point when given\n"
    "goto x y z          moves the party to that point here";
constexpr std::string_view kSpawnUsage =
    "spawn <enemy> [tier] [count] [map]            so many of the swarm's kind, at a tier\n"
    "spawn generator <enemy>|special [tier] [map]  a generator breeding that kind, or the "
    "temple's or underworld's mixed brood\n"
    "spawn golem|general|gargoyle [form] [map]     a great one; a gargoyle's form is eagl, lion "
    "or serp\n"
    "spawn item <name>                             a pickup of this level, by its record\n"
    "spawn object <name> [map]                     a copy of a level's object\n"
    "A map (E2) brings the kind's voices, a great one's costume or the object from that level; "
    "list enemies, critters, items and objects name what there is.";

/** The words a Legends map's preview admits. */
constexpr std::array<std::string_view, 2> kPreviewWords{"help", "goto"};

const ConsoleSession kOffline{};
const ConsoleSession kGuest{.role = ConsoleRole::Guest};
const ConsoleSession kHost{.role = ConsoleRole::Host};

/** A sink of the test's own: it keeps the lines it is given and answers each as told. */
class RecordingSink final : public ConsoleActionSink {
public:
    std::optional<std::string> submit(std::string line) override {
        lines.push_back(std::move(line));
        return answer;
    }
    /** The lines given since the last take. */
    std::vector<std::string> take() { return std::exchange(lines, {}); }

    std::vector<std::string> lines;
    std::optional<std::string> answer;
};

/** The application's console as it registers its commands, in help's order: stand-ins for the
 * local ones, the shared verbs over the test's sink, and the forms of get and goto, which keep
 * count of the words they answer. goto's form lists nothing, as on a machine without the
 * Legends maps. */
struct Console {
    DevConsole console;
    const LevelCatalog levels;
    RecordingSink sink;
    std::vector<std::string> informed; ///< what get info was asked of
    std::vector<std::string> legends;  ///< the Legends words goto's form answered

    Console() {
        const auto local = [this](std::string name, ConsoleReach reach) {
            console.addCommand(std::move(name), "stands in", reach,
                               [](DevConsole&, DevConsole::Arguments) {});
        };
        local("console_permissions", ConsoleReach::HostOnly);
        local("debug_info", ConsoleReach::Local);
        local("hidehud", ConsoleReach::Local);
        local("freecam", ConsoleReach::Local);
        local("list", ConsoleReach::Local);
        local("activate", ConsoleReach::OfflineOnly);
        local("unlock", ConsoleReach::OfflineOnly);
        local("lock", ConsoleReach::OfflineOnly);
        registerSharedConsoleCommands(console.host(), console.commands(), sink, levels);
        console.addForm("get", std::make_unique<LambdaForm>(
                                   ConsoleReach::Local, std::string{},
                                   [](DevConsole::Arguments arguments) {
                                       const auto gift = parseGift(arguments);
                                       return gift && gift->kind == GiftRequest::Kind::Info;
                                   },
                                   [this](DevConsole&, DevConsole::Arguments arguments) {
                                       informed.push_back(parseGift(arguments)->name);
                                   }));
        local("party_count", ConsoleReach::OfflineOnly);
        console.addForm("goto", std::make_unique<LambdaForm>(
                                    ConsoleReach::OfflineOnly, std::string{kLegendsOffline},
                                    [](DevConsole::Arguments arguments) {
                                        const auto destination = parseGoto(arguments);
                                        return destination && destination->offlineOnly();
                                    },
                                    [this](DevConsole&, DevConsole::Arguments arguments) {
                                        legends.push_back(arguments.front());
                                    }));
    }

    /** What the console shows under its echo of `line` once it has run it in `session`. */
    std::string say(std::string_view line, const ConsoleSession& session = kOffline) {
        console.setSession(session);
        const usize before = console.output().size();
        console.run(line);
        std::string shown;
        for (usize i = before + 1; i < console.output().size(); ++i) {
            shown += shown.empty() ? "" : "\n";
            shown += console.output()[i];
        }
        return shown;
    }

    const SharedConsoleCommand& command(std::string_view name) const {
        const auto* found =
            dynamic_cast<const SharedConsoleCommand*>(console.commands().find(name));
        REQUIRE(found != nullptr);
        return *found;
    }
};

/** What a fresh console shows of `text`, its lines joined by breaks. */
std::string shownAs(std::string_view text) {
    DevConsole console;
    console.print(text);
    std::string shown;
    for (const std::string& line : console.output()) {
        shown += shown.empty() ? "" : "\n";
        shown += line;
    }
    return shown;
}

/** A form of the test's own on goto, adding to help, the usage and the words while listed. */
class ListedForm final : public ConsoleForm {
public:
    explicit ListedForm(bool listed) : m_listed(listed) {}
    bool claims(Arguments arguments) const override {
        return !arguments.empty() && arguments.front() == "thing";
    }
    ConsoleReach reach() const override { return ConsoleReach::OfflineOnly; }
    void run(DevConsole& /*console*/, Arguments /*arguments*/) override {}
    std::string_view summaryForm() const override { return "<thing>"; }
    std::vector<std::pair<std::string, std::string>> usageRows() const override {
        return {{"goto thing", "goes to the thing"}};
    }
    std::string_view usageNote() const override { return "list things names them."; }
    ConsoleWords words(Arguments /*arguments*/) const override {
        return {.keywords = {"thing"}, .names = {"stuff"}};
    }
    bool listed() const override { return m_listed; }

private:
    bool m_listed;
};

TEST_CASE("a shared command sends its words as they were typed, after its name",
          "[dev-console][online-console]") {
    Console fixture;
    for (const ConsoleSession& session : {kOffline, kGuest, kHost}) {
        for (const auto& [line, sent] :
             std::to_array<std::pair<std::string_view, std::string_view>>(
                 {{"god", "god"},
                  {"GOD", "god"},
                  {"NoTarget", "notarget"},
                  {"noclip", "noclip"},
                  {"suicide", "suicide"},
                  {"unlock_tower", "unlock_tower"},
                  {"get Gold 5", "get Gold 5"},
                  {"get exp 10", "get exp 10"},
                  {"spawn Gru 2  3", "spawn Gru 2 3"},
                  {"spawn object Polysurf G1", "spawn object Polysurf G1"},
                  {"goto g1 1 2 3", "goto g1 1 2 3"},
                  {"goto Tower", "goto Tower"},
                  {"goto 1.5 -2 3", "goto 1.5 -2 3"}})) {
            CAPTURE(line);
            CHECK(fixture.say(line, session).empty());
            CHECK(fixture.sink.take() == std::vector<std::string>{std::string(sent)});
        }
    }
    // A $word stands for its value in the line sent.
    fixture.console.setVariable("ITEM", "gold");
    CHECK(fixture.say("GET $ITEM 100").empty());
    CHECK(fixture.sink.take() == std::vector<std::string>{"get gold 100"});
    // What the sink says of the line, the console shows.
    fixture.sink.answer = std::string(kRequested);
    CHECK(fixture.say("god", kGuest) == kRequested);
    CHECK(fixture.sink.take() == std::vector<std::string>{"god"});
}

TEST_CASE("online the shared game is asked only for the words it can read",
          "[dev-console][online-console]") {
    Console fixture;
    for (const ConsoleSession& session : {kGuest, kHost}) {
        CHECK(fixture.say("god x", session) == "Usage: god");
        CHECK(fixture.say("notarget now", session) == "Usage: notarget");
        CHECK(fixture.say("noclip 1", session) == "Usage: noclip");
        CHECK(fixture.say("suicide x", session) == "Usage: suicide");
        CHECK(fixture.say("unlock_tower please", session) == "Usage: unlock_tower");
        CHECK(fixture.say("goto lab", session) == kLegendsOffline);
        CHECK(fixture.say("goto back", session) == kLegendsOffline);
        CHECK(fixture.say("goto n64 castle", session) == kLegendsOffline);
        CHECK(fixture.say("goto ZZ9", session) == kLegendsOffline);
        CHECK(fixture.say("goto", session) == shownAs(kGotoUsage));
        CHECK(fixture.say("goto 1 2", session) == shownAs(kGotoUsage));
        CHECK(fixture.say("goto n64 1 2 3", session) == shownAs(kGotoUsage));
        CHECK(fixture.say("get", session) == shownAs(kGetUsage));
        CHECK(fixture.say("get banana", session) == "No item named banana; list items names them");
        CHECK(fixture.say("get exp 0", session) == "get exp <points>: how much to add");
        CHECK(fixture.say("spawn", session) == shownAs(kSpawnUsage));
        CHECK(fixture.say("spawn gru 4", session) == "A tier is 1 to 3");
        CHECK(fixture.say("spawn enemy zz", session) ==
              "No enemy kind named ZZ; list enemies names them");
    }
    CHECK(fixture.sink.take().empty());
    CHECK(fixture.legends.empty());
}

TEST_CASE("offline the shared verbs take the words they read none of, to refuse them themselves",
          "[dev-console][online-console]") {
    Console fixture;
    for (const std::string_view line :
         {"god x", "notarget now", "noclip 1", "suicide x", "unlock_tower please"}) {
        CAPTURE(line);
        CHECK(fixture.say(line).empty());
        CHECK(fixture.sink.take() == std::vector<std::string>{std::string(line)});
    }
    // The words no verb could read are answered here, as online.
    CHECK(fixture.say("goto") == shownAs(kGotoUsage));
    CHECK(fixture.say("goto n64 1 2 3") == shownAs(kGotoUsage));
    CHECK(fixture.say("get banana") == "No item named banana; list items names them");
    CHECK(fixture.say("spawn") == shownAs(kSpawnUsage));
    CHECK(fixture.sink.take().empty());
}

TEST_CASE("get info and the Legends maps are answered by their forms, never sent",
          "[dev-console][online-console]") {
    Console fixture;
    for (const ConsoleSession& session : {kOffline, kGuest, kHost}) {
        CHECK(fixture.say("get info", session).empty());
        CHECK(fixture.say("get INFO enemy#3", session).empty());
    }
    CHECK(fixture.informed ==
          std::vector<std::string>{"", "enemy#3", "", "enemy#3", "", "enemy#3"});
    CHECK(fixture.say("goto lab").empty());
    CHECK(fixture.say("goto back").empty());
    CHECK(fixture.say("goto n64 castle").empty());
    CHECK(fixture.legends == std::vector<std::string>{"lab", "back", "n64"});
    CHECK(fixture.say("goto lab", kGuest) == kLegendsOffline);
    CHECK(fixture.legends.size() == 3);
    CHECK(fixture.sink.take().empty());
}

TEST_CASE("a parked scene takes no shared words, though help and the usage still offer them",
          "[dev-console][online-console]") {
    Console fixture;
    const ConsoleSession parked =
        ConsolePolicy::session({.preview = kPreviewWords, .parked = true});
    REQUIRE(parked.hold == SceneHold::Parked);
    CHECK(fixture.say("goto G1", parked) == ConsolePolicy::kRefused);
    CHECK(fixture.say("goto 1 2 3", parked) == ConsolePolicy::kRefused);
    CHECK(fixture.say("goto n64 1 2 3", parked) == shownAs(kGotoUsage));
    CHECK(fixture.say("goto lab", parked).empty());
    CHECK(fixture.legends == std::vector<std::string>{"lab"});
    // What the preview does not admit is refused before it is read.
    CHECK(fixture.say("god", parked) == ConsolePolicy::kRefused);
    CHECK(fixture.say("god x", parked) == ConsolePolicy::kRefused);
    CHECK(fixture.sink.take().empty());
    const std::string help = fixture.say("help", parked);
    CHECK(help.contains("goto  "));
    CHECK_FALSE(help.contains("god  "));
}

TEST_CASE("the shared commands' help, usage and words read as they do today",
          "[dev-console][online-console]") {
    Console fixture;
    // help lists them where they are registered, padded to the widest name, the host's own.
    struct Help {
        std::string_view name;
        std::string_view summary;
    };
    constexpr std::array<Help, 17> kHelp{{
        {"help", "lists the commands"},
        {"debug_info", "stands in"},
        {"hidehud", "stands in"},
        {"freecam", "stands in"},
        {"list", "stands in"},
        {"activate", "stands in"},
        {"unlock", "stands in"},
        {"lock", "stands in"},
        {"goto", "<map> [x y z] or x y z: brings the party there"},
        {"unlock_tower", "opens every wing and portal of the tower"},
        {"get", "<item> [amount], exp <points> or level <level>: for you"},
        {"spawn", "stands enemies, great ones, generators, pickups or objects before you; spawn "
                  "alone says how"},
        {"suicide", "fells everyone standing"},
        {"notarget", "the enemies stop seeing you, or see you again"},
        {"god", "nothing harms you, or everything does again"},
        {"noclip", "flies you through everything, or sets you down again"},
        {"party_count", "stands in"},
    }};
    std::string expected;
    for (const Help& help : kHelp) {
        expected += expected.empty() ? "" : "\n";
        expected += shownAs(std::format("{:<19}  {}", help.name, help.summary));
    }
    CHECK(fixture.say("help") == expected);
    CHECK(fixture.command("get").usage() == kGetUsage);
    CHECK(fixture.command("goto").usage() == kGotoUsage);
    CHECK(fixture.command("spawn").usage() == kSpawnUsage);
    CHECK(fixture.command("god").reach() == ConsoleReach::Shared);
    // goto completes its first word to the tower and the catalogue's levels; the verbs that
    // read no words offer none.
    const std::vector<std::string> typing{""};
    const ConsoleRegistry& commands = fixture.console.commands();
    CHECK(commands.completions(fixture.command("goto"), typing) ==
          std::vector<std::string>{"tower"});
    CHECK(fixture.command("goto").words(typing).keywords == std::vector<std::string>{"tower"});
    CHECK(commands.completions(fixture.command("god"), typing).empty());
}

TEST_CASE("the forms listed on a shared verb join its help, its usage and its words",
          "[dev-console][online-console]") {
    for (const bool listed : {true, false}) {
        CAPTURE(listed);
        DevConsole console;
        RecordingSink sink;
        const LevelCatalog levels;
        registerSharedConsoleCommands(console.host(), console.commands(), sink, levels);
        console.addForm("goto", std::make_unique<ListedForm>(listed));
        const auto* command =
            dynamic_cast<const SharedConsoleCommand*>(console.commands().find("goto"));
        REQUIRE(command != nullptr);
        const std::vector<std::string> typing{""};
        if (listed) {
            CHECK(command->summary() == "<map> [x y z], x y z or <thing>: brings the party there");
            CHECK(command->usage() ==
                  "goto <map> [x y z]  travels to a level, standing at a point when given\n"
                  "goto x y z          moves the party to that point here\n"
                  "goto thing          goes to the thing\n"
                  "list things names them.");
            // The verb's grammar words, the form's, the verb's names, then the form's.
            CHECK(console.commands().completions(*command, typing) ==
                  std::vector<std::string>{"tower", "thing", "stuff"});
        } else {
            CHECK(command->summary() == "<map> [x y z] or x y z: brings the party there");
            CHECK(command->usage() == kGotoUsage);
            CHECK(console.commands().completions(*command, typing) ==
                  std::vector<std::string>{"tower"});
        }
        // Listed or not, the form claims its words.
        const std::vector<std::string> thing{"thing"};
        CHECK(console.commands().claimant("goto", thing) != nullptr);
    }
}

TEST_CASE("the console's session says which game its lines run in",
          "[dev-console][online-console]") {
    using Play = PlayScene::ConsolePlay;
    CHECK(consolePlayOf(kOffline) == Play::Offline);
    CHECK(consolePlayOf({.hold = SceneHold::Parked, .modal = kPreviewWords}) == Play::Offline);
    CHECK(consolePlayOf(kHost) == Play::Shared);
    CHECK(consolePlayOf(kGuest) == Play::Shared);
    CHECK(consolePlayOf({.role = ConsoleRole::NetplayMenus}) == Play::Shared);
}

} // namespace
