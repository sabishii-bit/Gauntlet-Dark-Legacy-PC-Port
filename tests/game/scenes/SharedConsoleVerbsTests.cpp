#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Expected.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/console/ConsoleReach.h"
#include "game/console/ConsoleUsage.h"
#include "game/console/ConsoleWords.h"
#include "game/console/GiftRequest.h"
#include "game/console/GotoRequest.h"
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
    CHECK(refusalOf("goto") == ConsoleRefusal{.kind = Kind::Usage, .text = sharedGotoUsage()});
    CHECK(refusalOf("goto 1 2") == ConsoleRefusal{.kind = Kind::Usage, .text = sharedGotoUsage()});
    CHECK(refusalOf("goto n64 1 2 3") ==
          ConsoleRefusal{.kind = Kind::Usage, .text = sharedGotoUsage()});
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

TEST_CASE("offline play sends the words a verb reads none of, and judges the rest alike",
          "[online-console][console-input]") {
    const auto offline = [](std::string_view line) {
        const std::vector<std::string> words = splitConsoleWords(line);
        REQUIRE_FALSE(words.empty());
        return verbNamed(words.front())
            .check(std::span<const std::string>{words}.subspan(1), Play::Offline);
    };
    for (const std::string_view name : {"god", "notarget", "noclip", "suicide", "unlock_tower"}) {
        CAPTURE(name);
        for (const std::string& line : {std::string(name), std::format("{} x", name)}) {
            const Reach reach = offline(line);
            REQUIRE(reach);
            CHECK(*reach == ConsoleReach::Shared);
        }
    }
    for (const std::string_view line :
         {"get", "get banana", "get gold 5", "get info", "goto", "goto 1 2", "goto lab", "goto G1",
          "spawn", "spawn gru 4", "spawn gru 2 3"}) {
        CAPTURE(line);
        const Reach reach = offline(line);
        const Reach shared = checked(line);
        REQUIRE(reach.has_value() == shared.has_value());
        if (reach) {
            CHECK(*reach == *shared);
        } else {
            CHECK(reach.error() == shared.error());
        }
    }
}

TEST_CASE("console places and flights stay finite and within a million of the origin",
          "[online-console][console-input]") {
    constexpr f32 kFarthest = 1.0e6f;
    constexpr f32 kInfinity = std::numeric_limits<f32>::infinity();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 beyond = std::nextafter(kFarthest, kInfinity);
    CHECK(consolePointInRange(Vec3{0.0f}));
    CHECK(consolePointInRange(Vec3{kFarthest}));
    CHECK(consolePointInRange(Vec3{-kFarthest}));
    CHECK(consolePointInRange(Vec3{kFarthest, -kFarthest, kFarthest}));
    for (s32 axis = 0; axis < 3; ++axis) {
        CAPTURE(axis);
        for (const f32 outside : {beyond, -beyond, nan, kInfinity, -kInfinity}) {
            CAPTURE(outside);
            Vec3 point{0.0f};
            point[axis] = outside;
            CHECK_FALSE(consolePointInRange(point));
        }
    }
}

TEST_CASE("each verb's help reads as the console's", "[dev-console][online-console]") {
    struct Help {
        std::string_view name;
        std::string_view summary;
    };
    // goto names its own forms; the Legends maps' form adds its own where it is listed.
    constexpr std::array<Help, 8> kHelp{
        {{"goto", "<map> [x y z] or x y z: brings the party there"},
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
    CHECK(usageOf(verbNamed("spawn")) == spawnUsage());
    const std::vector<std::pair<std::string, std::string>> gotoRows(GotoRequest::kUsageRows.begin(),
                                                                    GotoRequest::kUsageRows.end());
    CHECK(verbNamed("goto").usageRows() == gotoRows);
    CHECK(verbNamed("goto").usageNote().empty());
    for (const std::string_view name : {"god", "notarget", "noclip", "suicide", "unlock_tower"}) {
        CAPTURE(name);
        CHECK(verbNamed(name).usageRows().empty());
        CHECK(verbNamed(name).usageNote().empty());
    }
    // goto offers the tower for its first word, then the catalogue's levels; nothing else
    // offers a word.
    const LevelCatalog levels;
    const std::vector<std::string> typing{""};
    const std::vector<std::string> second{"G1", ""};
    const ConsoleWords first = verbNamed("goto").words(typing, levels);
    CHECK(first.keywords == std::vector<std::string>{"tower"});
    CHECK(first.names.empty());
    const ConsoleWords after = verbNamed("goto").words(second, levels);
    CHECK(after.keywords.empty());
    CHECK(after.names.empty());
    for (const SharedConsoleVerb* verb : sharedConsoleVerbs()) {
        if (verb->name() != "goto") {
            CAPTURE(verb->name());
            const ConsoleWords none = verb->words(typing, levels);
            CHECK(none.keywords.empty());
            CHECK(none.names.empty());
        }
    }
}

} // namespace
