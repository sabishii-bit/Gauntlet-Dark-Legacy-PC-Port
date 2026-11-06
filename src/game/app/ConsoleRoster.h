#pragma once
#include <functional>

#include "game/app/InspectConsoleForm.h"
#include "game/app/OnlineConsoleCommands.h"
#include "game/app/PartyCountCommand.h"
#include "game/app/ProfileConsoleCommands.h"
#include "game/app/ViewConsoleCommands.h"
#include "game/console/ConsoleActionSink.h"
#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleHost.h"
#include "game/console/ConsoleListings.h"
#include "game/scenes/FixtureConsoleCommands.h"
#include "game/scenes/LevelListings.h"

namespace gdl::game {

class LevelCatalog;

/** What the shared verbs are registered over. */
struct SharedVerbAccess {
    ConsoleActionSink& sink; ///< where their checked lines go
    const LevelCatalog& levels;
};

/** What every owner of the console's words reads and changes of the game. */
// `shared` is references alone, which every initialization must bind: the check counts the
// default constructor they delete as one that would leave them unset.
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
struct ConsoleRosterAccess {
    OnlineConsoleAccess online;
    ViewConsoleAccess view;
    LevelListingsAccess listings;
    FixtureConsoleAccess fixtures;
    SharedVerbAccess shared;
    InspectConsoleAccess inspect;
    PartyCountAccess partyCount;
    ProfileConsoleAccess profile;
};

/** The registrations the expansions add after the game's own. */
using ConsoleExpansions = std::function<void(ConsoleHost& host, ConsoleListings& listings)>;

/**
 * Registers the console's words with `host`, owner by owner, in the order help lists them
 * after its own: the online host's command, the view's, the level's listings and `list`, the
 * level's fixtures, the shared verbs in the wire's order with get info's form, party_count,
 * the profile's forms and listing, then `expansions`. `registry` is the console's, which the
 * shared verbs read their forms from; `listings` is what `list` reads, and must outlive it.
 */
void registerConsoleRoster(ConsoleHost& host, const ConsoleRegistry& registry,
                           ConsoleListings& listings, const ConsoleRosterAccess& access,
                           const ConsoleExpansions& expansions);

} // namespace gdl::game
