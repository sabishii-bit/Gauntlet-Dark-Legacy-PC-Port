#include "game/app/PartyCountCommand.h"

#include <format>
#include <utility>

#include "game/console/DevConsole.h"
#include "game/console/PartyCountRequest.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

PartyCountCommand::PartyCountCommand(PartyCountAccess access)
    : m_access(std::move(access)), m_words(wordsCompleter({"1", "2", "3", "4", "off"})) {}

std::string PartyCountCommand::summary() const {
    return "<1-4> or off: the party size the level sets itself up for";
}

void PartyCountCommand::run(DevConsole& console, Arguments arguments) {
    // A level sets itself up as it opens: the one in play keeps the enemies it placed.
    const auto describe = [&] {
        return m_access.count.has_value()
                   ? std::format("Levels are set up for a party of {} as they open",
                                 *m_access.count)
                   : std::string("Levels are set up for the players present as they open");
    };
    if (arguments.empty()) {
        console.print(describe());
        return;
    }
    const auto parsed = parsePartyCount(arguments);
    if (!parsed.has_value()) {
        console.print(parsed.error());
        return;
    }
    m_access.count = parsed->mode == PartyCountRequest::Mode::Fixed ? std::optional{parsed->players}
                                                                    : std::nullopt;
    if (PlayScene* scene = m_access.scene()) {
        scene->setPartyCountOverride(m_access.count);
    }
    console.print(describe());
}

std::vector<std::string> PartyCountCommand::completions(Arguments arguments) const {
    return m_words(arguments);
}

} // namespace gdl::game
