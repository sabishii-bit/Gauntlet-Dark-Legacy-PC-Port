#pragma once

#include "game/netplay/MatchSession.h"
#include "game/screens/SessionInputs.h"

namespace gdl::game {
/** Local device slots 0..N-1 map to this machine's room seats in ascending order.
 * Gameplay still sees the shared four-seat array; pointer/text/settings input
 * remains local and is never retained across the input playout delay. */
class MatchInputs {
public:
    static bool sample(MatchSession& session, const SessionInputs::Frame& local);
    static std::optional<SessionInputs::Frame> advance(MatchSession& session);
};
} // namespace gdl::game
