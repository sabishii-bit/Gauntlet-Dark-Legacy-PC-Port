#pragma once
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/console/ConsoleCommand.h"
#include "game/console/ConsoleReach.h"

namespace gdl::game {

class PlayScene;

/** What party_count reads and changes of the game, each read as it runs. */
struct PartyCountAccess {
    std::optional<s32>& count; ///< the size set across levels; none: the party present
    /** Offline play's scene while it is open; null with nothing in play. */
    std::function<PlayScene*()> scene;
};

/** party_count: the party size the level sets itself up for, or off, in offline play alone;
 * the scene in play takes it at once and every level opened after it. */
class PartyCountCommand final : public ConsoleCommand {
public:
    explicit PartyCountCommand(PartyCountAccess access);

    std::string_view name() const override { return "party_count"; }
    std::string summary() const override;
    ConsoleReach reach() const override { return ConsoleReach::OfflineOnly; }
    void run(DevConsole& console, Arguments arguments) override;
    std::vector<std::string> completions(Arguments arguments) const override;

private:
    PartyCountAccess m_access;
    LambdaCommand::Completer m_words; ///< the sizes and off, for the first word
};

} // namespace gdl::game
