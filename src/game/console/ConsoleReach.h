#pragma once
#include "engine/core/Types.h"

namespace gdl::game {

/** Where a console command or form runs, which decides the sessions that admit it. */
enum class ConsoleReach : u8 {
    Local,       ///< on the asking machine alone, in any session the console is open to
    OfflineOnly, ///< only in offline play: the level's own state, which no peer shares
    Shared,      ///< in the game every peer steps alike
    HostOnly,    ///< only on the online host
};

} // namespace gdl::game
