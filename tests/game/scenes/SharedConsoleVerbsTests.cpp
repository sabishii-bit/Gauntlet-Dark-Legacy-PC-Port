#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Expected.h"
#include "engine/core/Types.h"

#include "game/console/ConsoleCommands.h"
#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleUsage.h"
#include "game/console/GiftRequest.h"
#include "game/console/SpawnRequest.h"
#include "game/level/LevelCatalog.h"
#include "game/rollback/ConsoleAction.h"
#include "game/rollback/SharedConsoleWords.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/SharedConsoleVerbs.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Play = PlayScene::ConsolePlay;
using Kind = ConsoleRefusal::Kind;
using Reach = gdl::Expected<ConsoleReach, ConsoleRefusal>;

constexpr std::string_view kLegendsOffline = "Legends maps are offline only.";

const SharedConsoleVerb& verbNamed(std::string_view name) {
    const SharedConsoleVerb* verb = sharedConsoleVerb(name);
    REQUIRE(verb != nullptr);
    return *verb;
}

/** How every peer judges `line` before it is sent, its first word naming the verb. */
Reach checked(std::string_view line) {
    const std::vector<std::string> words = splitConsoleWords(line);
    REQUIRE_FALSE(words.empty());
    return verbNamed(words.front())
        .check(std::span<const std::string>{words}.subspan(1), Play::Shared);
}

/** Where `line` runs, which it must be admitted to do. */
ConsoleReach reachOf(std::string_view line) {
    const Reach reach = checked(line);
    REQUIRE(reach);
    return *reach;
}

/** Why `line` is refused, which it must be. */
ConsoleRefusal refusalOf(std::string_view line) {
    const Reach reach = checked(line);
    REQUIRE_FALSE(reach);
    return reach.error();
}

/** The help line a verb's forms and tail make: the forms joined by ", " and a last " or ". */
std::string summaryOf(const SharedConsoleVerb& verb) {
    const std::vector<std::string_view> forms = verb.summaryForms();
    if (forms.empty()) {
        return std::string(verb.summaryTail());
    }
    std::string text;
    for (usize i = 0; i < forms.size(); ++i) {
        text += i == 0 ? "" : (i + 1 == forms.size() ? " or " : ", ");
        text += forms[i];
    }
    return std::format("{}: {}", text, verb.summaryTail());
}

/** The usage a verb's rows and note make. */
std::string usageOf(const SharedConsoleVerb& verb) {
    const std::vector<std::pair<std::string, std::string>> rows = verb.usageRows();
    const std::vector<std::pair<std::string_view, std::string_view>> views(rows.begin(),
                                                                           rows.end());
    const std::array<std::string_view, 1> notes{verb.usageNote()};
    return consoleUsage(views, notes);
}

TEST_CASE("the scene's verbs are the wire's words", "[online-console][console-input]") {
    const auto verbs = sharedConsoleVerbs();
    REQUIRE(verbs.size() == kSharedConsoleWords.size());
    for (usize i = 0; i < verbs.size(); ++i) {
        const SharedConsoleWord& word = kSharedConsoleWords[i];
        CAPTURE(word.name);
        CHECK(verbs[i]->name() == word.name);
        CHECK(sharedConsoleVerb(word.name) == verbs[i]);
        CHECK(ConsoleAction{0, std::string(word.name)}.valid());
        // Only a local form is judged to run here, and the wire never carries one.
        const std::vector<std::string> local =
            word.localForm.empty() ? std::vector<std::string>{}
                                   : std::vector<std::string>{std::string(word.localForm)};
        const Reach reach = verbs[i]->check(local, Play::Shared);
        CHECK((reach && *reach == ConsoleReach::Local) == !word.localForm.empty());
        if (!word.localForm.empty()) {
            CHECK_FALSE(ConsoleAction{0, std::format("{} {}", word.name, word.localForm)}.valid());
        }
    }
    CHECK(sharedConsoleVerb("help") == nullptr);
    CHECK(sharedConsoleVerb("GOD") == nullptr);
}

TEST_CASE("every peer judges a verb's words alike before they are sent",
          "[online-console][console-input]") {
    // A verb that reads no words is refused any, by its name.
    for (const std::string_view name : {"god", "notarget", "noclip", "suicide", "unlock_tower"}) {
        CAPTURE(name);
        CHECK_FALSE(verbNamed(name).takesArguments());
        CHECK(reachOf(name) == ConsoleReach::Shared);
        CHECK(refusalOf(std::format("{} x", name)) ==
              ConsoleRefusal{.kind = Kind::Message, .text = std::format("Usage: {}", name)});
        CHECK(verbNamed(name).offlineOnlyRefusal().empty());
    }
    // get: its usage, an item it does not know, and info kept local.
    CHECK(verbNamed("get").takesArguments());
    CHECK(refusalOf("get") == ConsoleRefusal{.kind = Kind::Usage, .text = giftUsage()});
    CHECK(refusalOf("get banana") ==
          ConsoleRefusal{.kind = Kind::Message,
                         .text = "No item named banana; list items names them"});
    CHECK(reachOf("get gold 5") == ConsoleReach::Shared);
    CHECK(reachOf("get info") == ConsoleReach::Local);
    CHECK(reachOf("get info enemy#3") == ConsoleReach::Local);
    // goto: its usage, and the Legends maps only offline.
    CHECK(refusalOf("goto") ==
          ConsoleRefusal{.kind = Kind::Usage, .text = ConsoleCommands::gotoUsage()});
    CHECK(refusalOf("goto 1 2") ==
          ConsoleRefusal{.kind = Kind::Usage, .text = ConsoleCommands::gotoUsage()});
    CHECK(reachOf("goto lab") == ConsoleReach::OfflineOnly);
    CHECK(reachOf("goto back") == ConsoleReach::OfflineOnly);
    CHECK(reachOf("goto n64 castle") == ConsoleReach::OfflineOnly);
    CHECK(verbNamed("goto").offlineOnlyRefusal() == kLegendsOffline);
    CHECK(reachOf("goto G1") == ConsoleReach::Shared);
    CHECK(reachOf("goto 1 2 3") == ConsoleReach::Shared);
    CHECK(reachOf("goto Z9 1 2 3") == ConsoleReach::Shared);
    // spawn: its usage, and the words it reads but refuses.
    CHECK(refusalOf("spawn") == ConsoleRefusal{.kind = Kind::Usage, .text = spawnUsage()});
    CHECK(refusalOf("spawn gru 4") ==
          ConsoleRefusal{.kind = Kind::Message, .text = "A tier is 1 to 3"});
    CHECK(reachOf("spawn gru 2 3") == ConsoleReach::Shared);
    CHECK(verbNamed("spawn").offlineOnlyRefusal().empty());
}

TEST_CASE("each verb's help reads as the console's", "[dev-console][online-console]") {
    struct Help {
        std::string_view name;
        std::string_view summary;
    };
    constexpr std::array<Help, 8> kHelp{
        {{"goto", "<map> [x y z], x y z or <legends map> [preview]: brings the party there"},
         {"unlock_tower", "opens every wing and portal of the tower"},
         {"get", "<item> [amount], exp <points> or level <level>: for you"},
         {"spawn", "stands enemies, great ones, generators, pickups or objects before you; spawn "
                   "alone says how"},
         {"suicide", "fells everyone standing"},
         {"notarget", "the enemies stop seeing you, or see you again"},
         {"god", "nothing harms you, or everything does again"},
         {"noclip", "flies you through everything, or sets you down again"}}};
    for (const Help& help : kHelp) {
        CAPTURE(help.name);
        CHECK(summaryOf(verbNamed(help.name)) == help.summary);
    }
    CHECK(usageOf(verbNamed("get")) == giftUsage());
    CHECK(usageOf(verbNamed("goto")) == ConsoleCommands::gotoUsage());
    CHECK(usageOf(verbNamed("spawn")) == spawnUsage());
    for (const std::string_view name : {"god", "notarget", "noclip", "suicide", "unlock_tower"}) {
        CAPTURE(name);
        CHECK(verbNamed(name).usageRows().empty());
        CHECK(verbNamed(name).usageNote().empty());
    }
    // goto completes its first word to the tower and the catalogue's levels; nothing else
    // completes.
    const LevelCatalog levels;
    const std::vector<std::string> typing{""};
    const std::vector<std::string> second{"G1", ""};
    CHECK(verbNamed("goto").completions(typing, levels) == std::vector<std::string>{"tower"});
    CHECK(verbNamed("goto").completions(second, levels).empty());
    for (const SharedConsoleVerb* verb : sharedConsoleVerbs()) {
        if (verb->name() != "goto") {
            CAPTURE(verb->name());
            CHECK(verb->completions(typing, levels).empty());
        }
    }
}

} // namespace
