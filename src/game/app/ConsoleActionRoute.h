#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/Types.h"

#include "game/console/ConsoleActionSink.h"
#include "game/rollback/ConsoleAction.h"

namespace gdl::game {

/**
 * Where the console's checked shared lines go for the seat it speaks for: online, asked of the
 * shared game, saying whether it took them; offline, held for the next step of the scene in
 * play, whose replies are said once it has run them.
 */
class ConsoleActionRoute final : public ConsoleActionSink {
public:
    /** Asks the online game to run `line` for `seat`: whether it queued it, or none offline. */
    using Online = std::function<std::optional<bool>(s32 seat, std::string line)>;
    /** The seat the console speaks for; -1 with nobody in. */
    using Player = std::function<s32()>;

    ConsoleActionRoute(Online online, Player player);
    std::optional<std::string> submit(std::string line) override;
    /** The lines held offline since the last take, in the order they came. */
    std::vector<ConsoleAction> takeOffline();

private:
    Online m_online;
    Player m_player;
    std::vector<ConsoleAction> m_offline;
};

} // namespace gdl::game
