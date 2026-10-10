#include "game/screens/OnlineParty.h"

#include <algorithm>
#include <set>

namespace gdl::game {
bool OnlineParty::select(OnlineSession& session, std::span<const PartyMember> local) {
    if (local.empty() || local.size() > InputCommand::kSeats) {
        return false;
    }
    std::vector<PartyMember> choices(local.begin(), local.end());
    std::ranges::sort(choices, {}, &PartyMember::player);
    std::set<s32> devices;
    std::set<usize> slots;
    std::vector<CharacterProfile> profiles;
    for (const auto& choice : choices) {
        if (choice.player < 0 || choice.player >= static_cast<s32>(InputCommand::kSeats) ||
            !devices.insert(choice.player).second ||
            (choice.slot && !slots.insert(*choice.slot).second)) {
            return false;
        }
        const auto profile = CharacterProfile::capture(choice.save);
        if (!profile) {
            return false;
        }
        profiles.push_back(*profile);
    }
    if (!session.select(profiles)) {
        return false;
    }
    m_local = std::move(choices);
    m_session = &session;
    return true;
}
bool OnlineParty::admitted(const OnlineSession& session) const {
    return m_session == &session && session.phase() == OnlineSession::Phase::Active &&
           session.party() != nullptr && !m_local.empty() &&
           m_local.size() == session.seats().size();
}
bool OnlineParty::matches(const OnlineSession& session) const {
    if (!admitted(session)) {
        return false;
    }
    for (usize local = 0; local < m_local.size(); ++local) {
        const auto seat = session.seats()[local];
        const auto profile = CharacterProfile::capture(m_local[local].save);
        const auto& agreed = (*session.party())[seat];
        if (!profile || !agreed ||
            CharacterProfilePacket::encode(*profile) != CharacterProfilePacket::encode(*agreed)) {
            return false;
        }
    }
    return true;
}
std::optional<std::vector<PartyMember>> OnlineParty::members(const OnlineSession& session) const {
    if (!matches(session)) {
        return std::nullopt;
    }
    std::vector<PartyMember> result;
    for (usize seat = 0; seat < session.party()->size(); ++seat) {
        const auto& profile = (*session.party())[seat];
        if (!profile) {
            continue;
        }
        PartyMember member;
        member.player = static_cast<s32>(seat);
        const auto own = std::ranges::find(session.seats(), seat);
        if (own != session.seats().end()) {
            const auto local = static_cast<usize>(own - session.seats().begin());
            member.save = m_local[local].save;
            member.slot = m_local[local].slot;
        } else {
            member.save = profile->gameplayCopy();
        }
        result.push_back(std::move(member));
    }
    return result;
}
std::optional<SessionInputs::Frame> OnlineParty::inputs(const OnlineSession& session,
                                                        const SessionInputs::Frame& devices) const {
    if (!admitted(session)) {
        return std::nullopt;
    }
    SessionInputs::Frame frame;
    for (usize local = 0; local < m_local.size(); ++local) {
        frame[local] = devices[static_cast<usize>(m_local[local].player)];
    }
    return frame;
}
std::optional<s32> OnlineParty::device(const OnlineSession& session, u8 seat) const {
    if (!admitted(session)) {
        return std::nullopt;
    }
    const auto found = std::ranges::find(session.seats(), seat);
    if (found == session.seats().end()) {
        return std::nullopt;
    }
    return m_local[static_cast<usize>(found - session.seats().begin())].player;
}
} // namespace gdl::game
