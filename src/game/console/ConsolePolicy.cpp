#include "game/console/ConsolePolicy.h"

#include <algorithm>

namespace gdl::game {

ConsoleSession ConsolePolicy::session(const ConsoleSituation& situation) {
    ConsoleSession session;
    if (situation.online) {
        session.seat = situation.host ? ConsoleSeat::Host : ConsoleSeat::Guest;
        session.access = situation.allowed ? ConsoleAccess::Open : ConsoleAccess::Closed;
    } else if (situation.netplay) {
        session.seat = ConsoleSeat::NetplayMenus;
    }
    session.modal = situation.preview;
    return session;
}

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
