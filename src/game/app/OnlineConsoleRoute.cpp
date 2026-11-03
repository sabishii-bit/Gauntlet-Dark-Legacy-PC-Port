#include "game/app/OnlineConsoleRoute.h"

#include <format>
#include <string>
#include <utility>

#include "game/console/ConsoleCommands.h"
#include "game/console/GiftRequest.h"
#include "game/console/SpawnRequest.h"

namespace gdl::game {

OnlineConsoleRouting routeOnlineConsole(std::string_view name,
                                        ConsoleCommand::Arguments arguments) {
    if (name == "console_permissions" || name == "help" || name == "list" || name == "debug_info" ||
        name == "hidehud" || name == "freecam") {
        return {};
    }
    std::string error;
    if (name == "get") {
        const auto gift = parseGift(arguments);
        if (gift && gift->kind == GiftRequest::Kind::Info) {
            return {};
        }
        if (!gift) {
            error = gift.error();
        }
    } else if (name == "goto") {
        const auto destination = ConsoleCommands::parseGoto(arguments);
        if (!destination) {
            error = destination.error();
        } else if (destination->offlineOnly()) {
            error = "Legends maps are offline only.";
        }
    } else if (name == "spawn") {
        if (const auto spawn = parseSpawn(arguments); !spawn) {
            error = spawn.error();
        }
    } else if (!arguments.empty()) {
        error = std::format("Usage: {}", name);
    }
    if (!error.empty()) {
        return {.kind = OnlineConsoleRouting::Kind::Refused, .text = std::move(error)};
    }
    std::string line(name);
    for (const auto& argument : arguments) {
        line += ' ';
        line += argument;
    }
    return {.kind = OnlineConsoleRouting::Kind::Requested, .text = std::move(line)};
}

} // namespace gdl::game
