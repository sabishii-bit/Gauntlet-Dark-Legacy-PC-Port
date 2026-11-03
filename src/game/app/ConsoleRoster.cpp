#include "game/app/ConsoleRoster.h"

#include <memory>

#include "game/scenes/SharedConsoleCommand.h"

namespace gdl::game {

void registerConsoleRoster(ConsoleHost& host, const ConsoleRegistry& registry,
                           ConsoleListings& listings, const ConsoleRosterAccess& access,
                           const ConsoleExpansions& expansions) {
    // help is the console's own, registered before everything here.
    registerOnlineConsoleCommands(host, access.online);
    registerViewConsoleCommands(host, access.view);
    registerLevelListings(host, listings, access.listings);
    registerFixtureConsoleCommands(host, access.fixtures);
    registerSharedConsoleCommands(host, registry, access.shared.sink, access.shared.levels);
    host.addForm("get", std::make_unique<InspectConsoleForm>(access.inspect));
    host.add(std::make_unique<PartyCountCommand>(access.partyCount));
    registerProfileConsoleCommands(host, listings, access.profile);
    if (expansions) {
        expansions(host, listings);
    }
}

} // namespace gdl::game
