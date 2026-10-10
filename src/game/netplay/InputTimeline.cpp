#include "game/netplay/InputTimeline.h"

#include <limits>

#include "engine/core/Assert.h"

namespace gdl::game {

bool InputTimeline::assign(usize seat, Peer peer) {
    if (seat >= m_seats.size() || m_seats[seat].grant == std::numeric_limits<u32>::max()) {
        return false;
    }
    auto& state = m_seats[seat];
    state.owner = peer;
    ++state.grant;
    state.pending.clear();
    state.previous.reset();
    return true;
}

void InputTimeline::disconnect(Peer peer) {
    if (peer == 0) {
        return;
    }
    for (auto& state : m_seats) {
        if (state.owner == peer) {
            state.owner = 0;
            state.pending.clear();
            state.previous.reset();
        }
    }
}

InputTimeline::Peer InputTimeline::owner(usize seat) const {
    return seat < m_seats.size() ? m_seats[seat].owner : 0;
}

u32 InputTimeline::grant(usize seat) const {
    return seat < m_seats.size() ? m_seats[seat].grant : 0;
}

void InputTimeline::beginEpoch() {
    GDL_VERIFY(m_epoch < std::numeric_limits<u64>::max(), "Input epoch exhausted");
    ++m_epoch;
    m_tick = 0;
    for (auto& state : m_seats) {
        state.pending.clear();
        state.previous.reset();
    }
}

InputTimeline::Admission InputTimeline::submit(Peer sender, const InputCommand& command) {
    if (!command.valid()) {
        return Admission::Invalid;
    }
    if (command.epoch != m_epoch) {
        return Admission::WrongEpoch;
    }
    auto& state = m_seats[command.seat];
    if (sender == 0 || sender != state.owner) {
        return Admission::WrongOwner;
    }
    if (command.grant != state.grant) {
        return Admission::WrongGrant;
    }
    if (command.tick < m_tick) {
        return Admission::TooLate;
    }
    if (command.tick - m_tick > kMaxAhead) {
        return Admission::TooEarly;
    }
    // First accepted command wins: redundant packets cannot change a queued tick.
    return state.pending.emplace(command.tick, command).second ? Admission::Accepted
                                                               : Admission::Duplicate;
}

InputTimeline::Frame InputTimeline::advance() {
    GDL_VERIFY(m_tick < std::numeric_limits<u64>::max(), "Input tick exhausted");
    Frame result;
    for (usize seat = 0; seat < m_seats.size(); ++seat) {
        auto& state = m_seats[seat];
        auto& command = result[seat];
        const auto found = state.pending.find(m_tick);
        if (found != state.pending.end()) {
            state.previous = found->second;
            command = found->second;
            state.pending.erase(found);
        } else if (state.previous && m_tick - state.previous->tick <= kHoldTicks) {
            command = *state.previous;
            command.pressedButtons = 0;
        }
        command.epoch = m_epoch;
        command.tick = m_tick;
        command.grant = state.grant;
        command.seat = static_cast<u8>(seat);
    }
    ++m_tick;
    return result;
}

} // namespace gdl::game
