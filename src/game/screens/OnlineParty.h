#pragma once

#include "game/netplay/OnlineSession.h"
#include "game/players/Party.h"
#include "game/screens/SessionInputs.h"

namespace gdl::game {
/** Bridges the local select screen to the immutable online bootstrap. Device IDs,
 * room seats and disk slots remain separate: only this machine's choices retain
 * disk slots and unselected class history. This object never reads/writes saves. */
class OnlineParty {
public:
    bool select(OnlineSession& session, std::span<const PartyMember> local);
    /** Initial authoritative party (host) or presentation roster (guest). Requires
     * the acknowledged party; later gameplay progress belongs to the host. */
    std::optional<std::vector<PartyMember>> members(const OnlineSession& session) const;
    /** Reorder physical devices into the compact local-seat order MatchInputs
     * expects. Unselected local devices can never drive remote room seats. */
    std::optional<SessionInputs::Frame> inputs(const OnlineSession& session,
                                               const SessionInputs::Frame& devices) const;
    /** Resolve a locally owned room seat back to its physical input device. */
    std::optional<s32> device(const OnlineSession& session, u8 seat) const;
    void clear() {
        m_local.clear();
        m_session = nullptr;
    }

private:
    bool matches(const OnlineSession& session) const;
    bool admitted(const OnlineSession& session) const;
    const OnlineSession* m_session = nullptr; // identity only; never dereferenced
    std::vector<PartyMember> m_local;
};
} // namespace gdl::game
