#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/console/ConsolePolicy.h"
#include "game/console/ConsoleReach.h"

namespace {
using namespace gdl::game;

/** A command the application registers, at the reach it registers it with. */
struct Registered {
    std::string_view name;
    ConsoleReach reach;
};

/** The application's commands in help's order, the console's own help first. */
constexpr std::array<Registered, 18> kCommands{{
    {.name = "help", .reach = ConsoleReach::Local},
    {.name = "console_permissions", .reach = ConsoleReach::HostOnly},
    {.name = "debug_info", .reach = ConsoleReach::Local},
    {.name = "list", .reach = ConsoleReach::Local},
    {.name = "activate", .reach = ConsoleReach::OfflineOnly},
    {.name = "unlock", .reach = ConsoleReach::OfflineOnly},
    {.name = "lock", .reach = ConsoleReach::OfflineOnly},
    {.name = "goto", .reach = ConsoleReach::Local},
    {.name = "party_count", .reach = ConsoleReach::OfflineOnly},
    {.name = "unlock_tower", .reach = ConsoleReach::Local},
    {.name = "get", .reach = ConsoleReach::Local},
    {.name = "spawn", .reach = ConsoleReach::Local},
    {.name = "suicide", .reach = ConsoleReach::Local},
    {.name = "notarget", .reach = ConsoleReach::Local},
    {.name = "god", .reach = ConsoleReach::Local},
    {.name = "hidehud", .reach = ConsoleReach::Local},
    {.name = "freecam", .reach = ConsoleReach::Local},
    {.name = "noclip", .reach = ConsoleReach::Local},
}};

/** The words a Legends map's preview admits. */
constexpr std::array<std::string_view, 2> kPreviewWords{"help", "goto"};

/** What the application is doing, the session it hands the console, and what that admits. */
struct Row {
    std::string_view label;
    ConsoleSession session;
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
    const std::array<Row, 9> rows{{
        {.label = "offline", .session = {}, .admitted = everythingOffline},
        // The level's own commands are judged before the preview's words, so they stay.
        {.label = "offline, a Legends preview open",
         .session = {.modal = kPreviewWords},
         .admitted = {"help", "activate", "unlock", "lock", "goto", "party_count"}},
        {.label = "offline, walking a Legends map", .session = {}, .admitted = everythingOffline},
        {.label = "online host",
         .session = {.seat = ConsoleSeat::Host},
         .admitted = everythingHosted},
        {.label = "online host, the match not running",
         .session = {.seat = ConsoleSeat::Host, .access = ConsoleAccess::Closed},
         .admitted = {"console_permissions"}},
        {.label = "permitted guest",
         .session = {.seat = ConsoleSeat::Guest},
         .admitted = everythingOnline},
        {.label = "unpermitted guest",
         .session = {.seat = ConsoleSeat::Guest, .access = ConsoleAccess::Closed},
         .admitted = {}},
        {.label = "netplay menus",
         .session = {.seat = ConsoleSeat::NetplayMenus},
         .admitted = everythingOnline},
        // The online game outranks the menus it was entered from.
        {.label = "netplay menus with an online host",
         .session = {.seat = ConsoleSeat::Host},
         .admitted = everythingHosted},
    }};
    for (const Row& row : rows) {
        for (const Registered& command : kCommands) {
            INFO(row.label << ": " << command.name);
            const bool admitted =
                std::ranges::find(row.admitted, command.name) != row.admitted.end();
            const auto refused = ConsolePolicy::refusal(row.session, command.reach, command.name);
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
    // Under a preview an offline-only form still runs, and its own words are not needed.
    CHECK(ConsolePolicy::refusal({.modal = kPreviewWords}, ConsoleReach::OfflineOnly, "get",
                                 kOwn) == std::nullopt);
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
        for (const Registered& command : kCommands) {
            ConsoleSession parked = session;
            parked.hold = SceneHold::Parked;
            CHECK(ConsolePolicy::refusal(parked, command.reach, command.name) ==
                  ConsolePolicy::refusal(session, command.reach, command.name));
        }
    }
}

} // namespace
