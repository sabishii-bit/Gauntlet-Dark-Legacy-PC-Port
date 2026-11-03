#include "game/console/ConsolePolicy.h"

#include <algorithm>

namespace gdl::game {

std::optional<std::string_view> ConsolePolicy::refusal(const ConsoleSession& session,
                                                       ConsoleReach reach, std::string_view name,
                                                       std::string_view formRefusal) {
    // The level's own state is offline play's alone, whatever view holds the scene.
    if (reach == ConsoleReach::OfflineOnly) {
        if (session.seat == ConsoleSeat::Offline) {
            return std::nullopt;
        }
        return formRefusal.empty() ? kRefused : formRefusal;
    }
    if (!session.modal.empty()) {
        if (std::ranges::find(session.modal, name) != session.modal.end()) {
            return std::nullopt;
        }
        return kRefused;
    }
    if (reach == ConsoleReach::HostOnly) {
        if (session.seat == ConsoleSeat::Host) {
            return std::nullopt;
        }
        return kRefused;
    }
    // Everything else runs while the session's console is open.
    if (session.access == ConsoleAccess::Open) {
        return std::nullopt;
    }
    return kRefused;
}

} // namespace gdl::game
