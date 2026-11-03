#pragma once
#include <string>
#include <string_view>

#include "engine/core/Types.h"

#include "game/console/ConsoleCommand.h"

namespace gdl::game {

/** What the online console does with a command: runs it here, refuses it, or asks the shared
 * game to run it. */
struct OnlineConsoleRouting {
    enum class Kind : u8 { Local, Refused, Requested };
    Kind kind = Kind::Local;
    std::string text; ///< the refusal, or the line to request
};

/** Where the online console sends command `name` with `arguments`: here for every command but
 * the shared verbs, and for the words a shared verb keeps local (get info); refused for words
 * the shared game cannot read or runs only offline; else requested as one line of the name and
 * the arguments as typed. */
OnlineConsoleRouting routeOnlineConsole(std::string_view name, ConsoleCommand::Arguments arguments);

} // namespace gdl::game
