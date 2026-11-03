#pragma once
#include <array>
#include <string_view>

#include "game/console/ConsoleReach.h"

namespace gdl::test {

/** A command the application registers, at the reach it registers it with. */
struct RegisteredCommand {
    std::string_view name;
    game::ConsoleReach reach;
};

/** The application's commands in help's order, the console's own help first; the console's
 * roster tests hold the registrations to it. */
inline constexpr std::array<RegisteredCommand, 18> kRegisteredCommands{{
    {.name = "help", .reach = game::ConsoleReach::Local},
    {.name = "console_permissions", .reach = game::ConsoleReach::HostOnly},
    {.name = "debug_info", .reach = game::ConsoleReach::Local},
    {.name = "hidehud", .reach = game::ConsoleReach::Local},
    {.name = "freecam", .reach = game::ConsoleReach::Local},
    {.name = "list", .reach = game::ConsoleReach::Local},
    {.name = "activate", .reach = game::ConsoleReach::OfflineOnly},
    {.name = "unlock", .reach = game::ConsoleReach::OfflineOnly},
    {.name = "lock", .reach = game::ConsoleReach::OfflineOnly},
    {.name = "goto", .reach = game::ConsoleReach::Shared},
    {.name = "unlock_tower", .reach = game::ConsoleReach::Shared},
    {.name = "get", .reach = game::ConsoleReach::Shared},
    {.name = "spawn", .reach = game::ConsoleReach::Shared},
    {.name = "suicide", .reach = game::ConsoleReach::Shared},
    {.name = "notarget", .reach = game::ConsoleReach::Shared},
    {.name = "god", .reach = game::ConsoleReach::Shared},
    {.name = "noclip", .reach = game::ConsoleReach::Shared},
    {.name = "party_count", .reach = game::ConsoleReach::OfflineOnly},
}};

} // namespace gdl::test
