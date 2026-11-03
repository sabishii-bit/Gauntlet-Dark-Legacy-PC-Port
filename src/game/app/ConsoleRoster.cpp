#include "game/app/ConsoleRoster.h"

#include <memory>

#include "game/level/LevelCatalog.h"
#include "game/scenes/SharedConsoleCommand.h"

namespace gdl::game {

namespace {
/** The words goto completes to: the tower, the way back from a Legends map, the levels and
 * the Legends maps installed for the first word, and preview after a second. */
LambdaCommand::Completer gotoWords(const SharedVerbAccess& access) {
    return [&levels = access.levels,
            legendsMapNames = access.legendsMapNames](ConsoleCommand::Arguments arguments) {
        if (arguments.size() > 1) {
            return arguments.size() == 2 ? std::vector<std::string>{"preview"}
                                         : std::vector<std::string>{};
        }
        std::vector<std::string> words{"tower", "back"};
        for (const LevelCatalog::Realm& realm : levels.realms()) {
            words.insert(words.end(), realm.levels.begin(), realm.levels.end());
        }
        const std::vector<std::string> legends = legendsMapNames();
        words.insert(words.end(), legends.begin(), legends.end());
        return words;
    };
}
} // namespace

void registerConsoleRoster(ConsoleHost& host, const ConsoleRegistry& registry,
                           ConsoleListings& listings, const ConsoleRosterAccess& access,
                           const ConsoleExpansions& expansions) {
    // help is the console's own, registered before everything here.
    registerOnlineConsoleCommands(host, access.online);
    registerViewConsoleCommands(host, access.view);
    registerLevelListings(host, listings, access.listings);
    registerFixtureConsoleCommands(host, access.fixtures);
    registerSharedConsoleCommands(host, registry, access.shared.sink, access.shared.levels,
                                  gotoWords(access.shared));
    host.addForm("get", std::make_unique<InspectConsoleForm>(access.inspect));
    host.add(std::make_unique<PartyCountCommand>(access.partyCount));
    registerProfileConsoleCommands(host, listings, access.profile);
    if (expansions) {
        expansions(host, listings);
    }
}

} // namespace gdl::game
