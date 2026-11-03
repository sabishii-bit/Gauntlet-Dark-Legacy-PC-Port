#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/console/ConsolePolicy.h"
#include "game/console/ConsoleReach.h"
#include "game/console/RegisteredCommands.h"

namespace {
using namespace gdl::game;
using gdl::test::kRegisteredCommands;
using gdl::test::RegisteredCommand;

/** The words a Legends map's preview admits. */
constexpr std::array<std::string_view, 2> kPreviewWords{"help", "goto"};

/** What the application is doing, the facts it makes the console's session from, and what
 * that session admits. */
struct Row {
    std::string_view label;
    ConsoleSituation situation;
    std::vector<std::string_view> admitted;
};

TEST_CASE("each session admits what it admits today", "[dev-console][online-console]") {
    const std::vector<std::string_view> everythingOffline{
        "help",     "debug_info",  "list",         "activate", "unlock", "lock",
        "goto",     "party_count", "unlock_tower", "get",      "spawn",  "suicide",
        "notarget", "god",         "hidehud",      "freecam",  "noclip"};
    const std::vector<std::string_view> everythingOnline{
        "help",    "debug_info", "list", "goto",    "unlock_tower", "get",   "spawn",
        "suicide", "notarget",   "god",  "hidehud", "freecam",      "noclip"};
    std::vector<std::string_view> everythingHosted = everythingOnline;
    everythingHosted.insert(everythingHosted.begin() + 1, "console_permissions");
    const std::array<Row, 8> rows{{
        // Walking a level or a Legends map is offline play: neither is a fact of the session.
        {.label = "offline, walking a level or a Legends map",
         .situation = {},
         .admitted = everythingOffline},
        // The preview's words alone run under it, not even the level's own commands.
        {.label = "offline, a Legends preview open",
         .situation = {.preview = kPreviewWords},
         .admitted = {"help", "goto"}},
        {.label = "online host",
         .situation = {.online = true, .host = true, .allowed = true},
         .admitted = everythingHosted},
        {.label = "online host, the match not running",
         .situation = {.online = true, .host = true},
         .admitted = {"console_permissions"}},
        {.label = "permitted guest",
         .situation = {.online = true, .allowed = true},
         .admitted = everythingOnline},
        {.label = "unpermitted guest", .situation = {.online = true}, .admitted = {}},
        {.label = "netplay menus", .situation = {.netplay = true}, .admitted = everythingOnline},
        // The online game outranks the menus it was entered from.
        {.label = "netplay menus with an online host",
         .situation = {.online = true, .host = true, .allowed = true, .netplay = true},
         .admitted = everythingHosted},
    }};
    for (const Row& row : rows) {
        const ConsoleSession session = ConsolePolicy::session(row.situation);
        for (const RegisteredCommand& command : kRegisteredCommands) {
            INFO(row.label << ": " << command.name);
            const bool admitted =
                std::ranges::find(row.admitted, command.name) != row.admitted.end();
            const auto refused = ConsolePolicy::refusal(session, command.reach, command.name);
            CHECK(refused.has_value() != admitted);
            if (refused.has_value()) {
                CHECK(*refused == "This command is not available in this session.");
            }
        }
    }
}

TEST_CASE("an offline-only form refuses in its own words, and nothing else does",
          "[dev-console][online-console]") {
    constexpr std::string_view kOwn = "Stand-in words are offline only.";
    const ConsoleSession guest{.seat = ConsoleSeat::Guest};
    CHECK(ConsolePolicy::refusal(guest, ConsoleReach::OfflineOnly, "goto", kOwn) == kOwn);
    CHECK(ConsolePolicy::refusal(guest, ConsoleReach::OfflineOnly, "goto") ==
          ConsolePolicy::kRefused);
    CHECK(ConsolePolicy::refusal({}, ConsoleReach::OfflineOnly, "goto", kOwn) == std::nullopt);
    // Under a preview an offline-only form runs only on a word the preview admits, and is
    // refused otherwise in the preview's words, not its own.
    CHECK(ConsolePolicy::refusal({.modal = kPreviewWords}, ConsoleReach::OfflineOnly, "goto",
                                 kOwn) == std::nullopt);
    CHECK(ConsolePolicy::refusal({.modal = kPreviewWords}, ConsoleReach::OfflineOnly, "get",
                                 kOwn) == ConsolePolicy::kRefused);
    const ConsoleSession closed{.seat = ConsoleSeat::Guest, .access = ConsoleAccess::Closed};
    CHECK(ConsolePolicy::refusal(closed, ConsoleReach::Local, "goto", kOwn) ==
          ConsolePolicy::kRefused);
    CHECK(ConsolePolicy::refusal(guest, ConsoleReach::HostOnly, "goto", kOwn) ==
          ConsolePolicy::kRefused);
    CHECK(ConsolePolicy::refusal({.modal = kPreviewWords}, ConsoleReach::Local, "get", kOwn) ==
          ConsolePolicy::kRefused);
}

TEST_CASE("shared words are judged as local ones, and a parked scene changes no admission",
          "[dev-console][online-console]") {
    for (const ConsoleSession& session :
         {ConsoleSession{}, ConsoleSession{.seat = ConsoleSeat::Host},
          ConsoleSession{.seat = ConsoleSeat::Guest},
          ConsoleSession{.seat = ConsoleSeat::Guest, .access = ConsoleAccess::Closed},
          ConsoleSession{.seat = ConsoleSeat::NetplayMenus},
          ConsoleSession{.modal = kPreviewWords}}) {
        for (const std::string_view name : {"god", "goto", "help"}) {
            CHECK(ConsolePolicy::refusal(session, ConsoleReach::Shared, name) ==
                  ConsolePolicy::refusal(session, ConsoleReach::Local, name));
        }
        for (const RegisteredCommand& command : kRegisteredCommands) {
            ConsoleSession parked = session;
            parked.hold = SceneHold::Parked;
            CHECK(ConsolePolicy::refusal(parked, command.reach, command.name) ==
                  ConsolePolicy::refusal(session, command.reach, command.name));
        }
    }
}

TEST_CASE("a parked scene refuses shared words once they are read, and only a parked one",
          "[dev-console][online-console]") {
    // A Legends preview parks the scene it was opened over.
    const ConsoleSession preview =
        ConsolePolicy::session({.preview = kPreviewWords, .parked = true});
    CHECK(preview.hold == SceneHold::Parked);
    CHECK(preview.seat == ConsoleSeat::Offline);
    CHECK(ConsolePolicy::sharedRefusal(preview) == ConsolePolicy::kRefused);
    for (const ConsoleSituation& situation :
         {ConsoleSituation{}, ConsoleSituation{.preview = kPreviewWords},
          ConsoleSituation{.online = true, .host = true, .allowed = true},
          ConsoleSituation{.online = true, .allowed = true}, ConsoleSituation{.online = true},
          ConsoleSituation{.netplay = true}}) {
        const ConsoleSession session = ConsolePolicy::session(situation);
        CHECK(session.hold == SceneHold::Live);
        CHECK(ConsolePolicy::sharedRefusal(session) == std::nullopt);
        ConsoleSession parked = session;
        parked.hold = SceneHold::Parked;
        CHECK(ConsolePolicy::sharedRefusal(parked) == ConsolePolicy::kRefused);
    }
}

} // namespace
