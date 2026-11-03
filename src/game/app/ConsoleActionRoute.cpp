#include "game/app/ConsoleActionRoute.h"

#include <utility>

#include "engine/core/Assert.h"

namespace gdl::game {

ConsoleActionRoute::ConsoleActionRoute(Online online, Player player)
    : m_online(std::move(online)), m_player(std::move(player)) {}

std::optional<std::string> ConsoleActionRoute::submit(std::string line) {
    const s32 seat = m_player ? m_player() : -1;
    if (const std::optional<bool> queued = m_online ? m_online(seat, line) : std::nullopt) {
        return *queued
                   ? "Command requested for the shared game."
                   : "Command not queued: permission denied, session busy, or request too long.";
    }
    if (seat < 0) {
        return "Nobody is in.";
    }
    ConsoleAction action{.seat = static_cast<u8>(seat), .line = std::move(line)};
    // The console types only printable characters and checks the verb before a line comes here.
    GDL_VERIFY(action.admitted(), "an offline console line is admitted before it is held");
    m_offline.push_back(std::move(action));
    return std::nullopt;
}

std::vector<ConsoleAction> ConsoleActionRoute::takeOffline() {
    return std::exchange(m_offline, {});
}

} // namespace gdl::game
