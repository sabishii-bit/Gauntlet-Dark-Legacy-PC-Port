#include "game/console/ConsolePolicy.h"

#include <algorithm>

namespace gdl::game {

ConsoleSession ConsolePolicy::session(const ConsoleSituation& situation) {
    ConsoleSession session;
    if (situation.online) {
        session.role = situation.host ? ConsoleRole::Host : ConsoleRole::Guest;
        session.access = situation.allowed ? ConsoleAccess::Open : ConsoleAccess::Closed;
    } else if (situation.netplay) {
        session.role = ConsoleRole::NetplayMenus;
    }
    session.modal = situation.preview;
    session.hold = situation.parked ? SceneHold::Parked : SceneHold::Live;
    return session;
}

std::optional<std::string_view> ConsolePolicy::refusal(const ConsoleSession& session,
                                                       ConsoleReach reach, std::string_view name,
                                                       std::string_view formRefusal) {
    // Under a modal view, such as a Legends map's preview, only its words run, not even the
    // level's own.
    const bool modal = !session.modal.empty();
    if (modal && std::ranges::find(session.modal, name) == session.modal.end()) {
        return kRefused;
    }
    // The level's own state is offline play's alone.
    if (reach == ConsoleReach::OfflineOnly) {
        if (session.role == ConsoleRole::Offline) {
            return std::nullopt;
        }
        return formRefusal.empty() ? kRefused : formRefusal;
    }
    if (modal) {
        return std::nullopt;
    }
    if (reach == ConsoleReach::HostOnly) {
        if (session.role == ConsoleRole::Host) {
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

std::optional<std::string_view> ConsolePolicy::sharedRefusal(const ConsoleSession& session) {
    if (session.hold == SceneHold::Parked) {
        return kRefused;
    }
    return std::nullopt;
}

} // namespace gdl::game
