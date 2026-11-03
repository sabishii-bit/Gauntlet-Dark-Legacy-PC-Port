#include "game/app/OnlineConsoleRoute.h"

#include <string>
#include <utility>

#include "game/console/ConsoleReach.h"
#include "game/scenes/PlayScene.h"
#include "game/scenes/SharedConsoleVerbs.h"

namespace gdl::game {

OnlineConsoleRouting routeOnlineConsole(std::string_view name,
                                        ConsoleCommand::Arguments arguments) {
    const SharedConsoleVerb* verb = sharedConsoleVerb(name);
    if (verb == nullptr) {
        return {};
    }
    auto reach = verb->check(arguments, PlayScene::ConsolePlay::Shared);
    if (!reach) {
        return {.kind = OnlineConsoleRouting::Kind::Refused, .text = std::move(reach.error().text)};
    }
    if (*reach == ConsoleReach::Local) {
        return {};
    }
    if (*reach == ConsoleReach::OfflineOnly) {
        return {.kind = OnlineConsoleRouting::Kind::Refused,
                .text = std::string(verb->offlineOnlyRefusal())};
    }
    std::string line(name);
    for (const auto& argument : arguments) {
        line += ' ';
        line += argument;
    }
    return {.kind = OnlineConsoleRouting::Kind::Requested, .text = std::move(line)};
}

} // namespace gdl::game
