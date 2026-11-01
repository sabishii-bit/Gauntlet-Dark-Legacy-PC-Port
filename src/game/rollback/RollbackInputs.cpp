#include "game/rollback/RollbackInputs.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace gdl::game {
namespace {
bool sameReal(f32 a, f32 b) {
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}
bool same(const InputCommand& a, const InputCommand& b) {
    if (a.epoch != b.epoch || a.tick != b.tick || a.grant != b.grant || a.seat != b.seat ||
        !sameReal(a.direction.x, b.direction.x) || !sameReal(a.direction.y, b.direction.y) ||
        !sameReal(a.magnitude, b.magnitude) ||
        a.aimDirection.has_value() != b.aimDirection.has_value() ||
        a.heldButtons != b.heldButtons || a.pressedButtons != b.pressedButtons ||
        a.hintTopic != b.hintTopic || a.hintRevision != b.hintRevision || a.console != b.console ||
        !sameReal(a.consoleFlight.x, b.consoleFlight.x) ||
        !sameReal(a.consoleFlight.y, b.consoleFlight.y) ||
        !sameReal(a.consoleFlight.z, b.consoleFlight.z)) {
        return false;
    }
    return !a.aimDirection || (sameReal(a.aimDirection->x, b.aimDirection->x) &&
                               sameReal(a.aimDirection->y, b.aimDirection->y) &&
                               sameReal(a.aimDirection->z, b.aimDirection->z));
}
} // namespace

u8 RollbackInputs::leadForPing(s32 pingMs) {
    if (pingMs < 0) {
        return 1;
    }
    if (pingMs < kNoDelayPingMs) {
        return 0;
    }
    if (pingMs < kOneTickPingMs) {
        return 1;
    }
    if (pingMs < kTwoTickPingMs) {
        return 2;
    }
    return pingMs < kThreeTickPingMs ? 3 : kMaxDelay;
}

bool RollbackInputs::occupied(u8 peer) const {
    return peer > 0 && peer <= 4 && std::ranges::find(m_owners, peer) != m_owners.end();
}

bool RollbackInputs::open(u8 localPeer, u64 epoch, const Owners& owners, const Grants& grants,
                          u8 delay) {
    if (m_open || localPeer == 0 || localPeer > 4 || epoch == 0 || delay > kMaxDelay ||
        std::ranges::find(owners, localPeer) == owners.end()) {
        return false;
    }
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if (owners[seat] > 4 || (owners[seat] == 0) != (grants[seat] == 0)) {
            return false;
        }
    }
    m_open = true;
    m_local = localPeer;
    m_epoch = epoch;
    m_delay = delay;
    m_owners = owners;
    m_grants = grants;
    m_sample = 1 + delay;
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (occupied(peer)) {
            m_peerTicks[peer] = 1;
            // Before an origin's first measured report, zero transit would make
            // equal-rate peers idle after three ticks on an ordinary WAN link.
            // This affects pacing only; ready() still caps unconfirmed play at
            // eight ticks. The first known report replaces this allowance.
            if (peer != localPeer) {
                m_peerTransitTicks[peer] = static_cast<u8>(kPredictionTicks);
            }
        }
    }
    // The agreed local buffer begins with the same known neutral inputs on all
    // peers. This is not permission to fill a later missing frame with authority.
    for (u64 tick = 1; tick <= delay; ++tick) {
        auto& frame = m_actual[tick];
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
        for (u8 seat = 0; seat < static_cast<u8>(owners.size()); ++seat) {
            if (owners[seat] != 0) {
                InputCommand command;
                command.epoch = epoch;
                command.tick = tick;
                command.grant = grants[seat];
                command.seat = seat;
                frame[seat] = command;
            }
        }
    }
    m_actualThrough = delay;
    return true;
}

bool RollbackInputs::suspendedSeat(usize seat, u64 tick) const {
    if (seat >= m_owners.size()) {
        return false;
    }
    const u8 owner = m_owners[seat];
    return owner != 0 && m_suspendedFrom[owner] != 0 && tick >= m_suspendedFrom[owner];
}

InputCommand RollbackInputs::neutral(u8 seat, u64 tick) const {
    InputCommand command;
    command.epoch = m_epoch;
    command.tick = tick;
    command.grant = m_grants[seat];
    command.seat = seat;
    return command;
}

bool RollbackInputs::complete(u64 tick, const Actual& frame) const {
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        if (m_owners[seat] != 0 && !frame[seat] && !suspendedSeat(seat, tick)) {
            return false;
        }
    }
    return true;
}

bool RollbackInputs::suspend(u8 peer, u64 fromTick, u64 settledThrough) {
    if (!m_open || !occupied(peer) || peer == m_local || fromTick == 0) {
        return false;
    }
    if (m_suspendedFrom[peer] != 0) {
        return m_suspendedFrom[peer] == fromTick;
    }
    m_suspendedFrom[peer] = fromTick;
    // Ticks already executed on a prediction become actual neutral input, and so does input
    // a direct link brought here before the host had it: every peer holds the host's tick.
    // Whatever was used and was not neutral is corrected like any other misprediction.
    const u64 revisable = std::max(fromTick, settledThrough + 1);
    for (auto& [tick, actual] : m_actual) {
        if (tick < revisable) {
            continue;
        }
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if (m_owners[seat] == peer) {
                actual[seat] = neutral(static_cast<u8>(seat), tick);
            }
        }
    }
    for (const auto& [tick, used] : m_used) {
        if (tick < revisable) {
            continue;
        }
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if (m_owners[seat] == peer &&
                !same(used.commands[seat], neutral(static_cast<u8>(seat), tick))) {
                m_mismatch = m_mismatch ? std::min(*m_mismatch, tick) : tick;
            }
        }
    }
    // The seats leave play at fromTick too (NativeRollbackDriver), so the unsettled ticks
    // already simulated from there replay, even where the guess was right.
    if (const auto executed = m_used.lower_bound(revisable); executed != m_used.end()) {
        m_mismatch = m_mismatch ? std::min(*m_mismatch, executed->first) : executed->first;
    }
    while (m_actualThrough != std::numeric_limits<u64>::max()) {
        const auto next = m_actual.find(m_actualThrough + 1);
        if (next == m_actual.end() ? !complete(m_actualThrough + 1, Actual{})
                                   : !complete(m_actualThrough + 1, next->second)) {
            break;
        }
        ++m_actualThrough;
    }
    return true;
}

RollbackInputs::Admission RollbackInputs::admit(u8 peer, std::span<const InputCommand> commands) {
    if (!m_open || !occupied(peer) || commands.empty() ||
        commands.size() > kRedundantTicks * InputCommand::kSeats) {
        return Admission::Invalid;
    }
    std::map<u64, Actual> incoming;
    bool ignored = false;
    for (const auto& command : commands) {
        if (!command.valid() || command.tick == 0) {
            return Admission::Invalid;
        }
        if (m_suspendedFrom[peer] != 0 && command.tick >= m_suspendedFrom[peer]) {
            ignored = true; // a dropped peer's seats already hold neutral input here
            continue;
        }
        if (command.epoch != m_epoch) {
            return Admission::WrongEpoch;
        }
        if (m_owners[command.seat] != peer ||
            (command.console && (peer != 1 || m_owners[command.console->seat] == 0))) {
            return Admission::WrongPeer;
        }
        if (command.grant != m_grants[command.seat]) {
            return Admission::WrongGrant;
        }
        if (command.tick >= m_next && command.tick - m_next > kMaxAhead) {
            return Admission::TooEarly;
        }
        auto& slot = incoming[command.tick][command.seat];
        if (slot) {
            return same(*slot, command) ? Admission::Invalid : Admission::Conflict;
        }
        slot = command;
    }
    if (incoming.size() > kRedundantTicks) {
        return Admission::Invalid;
    }
    if (incoming.empty()) {
        return ignored ? Admission::Stale : Admission::Invalid;
    }
    bool newInput = false;
    bool retained = false;
    for (const auto& [tick, frame] : incoming) {
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if (frame[seat].has_value() != (m_owners[seat] == peer)) {
                return Admission::Invalid;
            }
        }
        const auto existing = m_actual.find(tick);
        if (tick < m_next && m_next - tick > kHistoryTicks && existing == m_actual.end()) {
            continue;
        }
        retained = true;
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if (!frame[seat]) {
                continue;
            }
            if (existing != m_actual.end() && existing->second[seat]) {
                if (!same(*frame[seat], *existing->second[seat])) {
                    return Admission::Conflict;
                }
            } else {
                newInput = true;
            }
        }
    }
    if (!newInput) {
        return retained ? Admission::Duplicate : Admission::Stale;
    }
    // Commit only after the entire bounded batch validates. In particular, a
    // conflicting trailing duplicate cannot smuggle earlier new inputs in.
    for (const auto& [tick, frame] : incoming) {
        if (tick < m_next && m_next - tick > kHistoryTicks && !m_actual.contains(tick)) {
            continue;
        }
        auto& actual = m_actual[tick];
        const auto used = m_used.find(tick);
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if (!frame[seat] || actual[seat]) {
                continue;
            }
            actual[seat] = frame[seat];
            if (used != m_used.end() && !same(*frame[seat], used->second.commands[seat])) {
                m_mismatch = m_mismatch ? std::min(*m_mismatch, tick) : tick;
            }
        }
    }
    while (m_actualThrough != std::numeric_limits<u64>::max()) {
        const auto next = m_actual.find(m_actualThrough + 1);
        if (next == m_actual.end() ? !complete(m_actualThrough + 1, Actual{})
                                   : !complete(m_actualThrough + 1, next->second)) {
            break;
        }
        ++m_actualThrough;
    }
    return Admission::Accepted;
}

bool RollbackInputs::sampleReady() const {
    return m_open && !replaying() && !m_mismatch &&
           m_next <= std::numeric_limits<u64>::max() - m_delay && m_sample <= m_next + m_delay &&
           m_sample != std::numeric_limits<u64>::max();
}

bool RollbackInputs::sample(std::span<const InputCommand> commands) {
    if (!sampleReady() || commands.empty() ||
        std::ranges::any_of(commands,
                            [&](const auto& command) { return command.tick != m_sample; }) ||
        admit(m_local, commands) != Admission::Accepted) {
        return false;
    }
    m_redundancy.insert(m_redundancy.end(), commands.begin(), commands.end());
    std::erase_if(m_redundancy,
                  [&](const auto& command) { return m_sample - command.tick >= kRedundantTicks; });
    ++m_sample;
    return true;
}

RollbackInputs::Admission RollbackInputs::submit(u8 authenticatedPeer,
                                                 std::span<const InputCommand> commands) {
    if (!occupied(authenticatedPeer) || authenticatedPeer == m_local) {
        return Admission::WrongPeer;
    }
    return admit(authenticatedPeer, commands);
}

std::vector<InputCommand> RollbackInputs::resendInputs(u64 firstTick) const {
    std::vector<InputCommand> result;
    if (!m_open || firstTick == 0) {
        return result;
    }
    auto frame = m_actual.find(firstTick);
    for (u64 count = 0; count < kRedundantTicks && frame != m_actual.end(); ++count, ++frame) {
        if (frame->first < firstTick || frame->first - firstTick != count) {
            break;
        }
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if (m_owners[seat] == m_local && !frame->second[seat]) {
                return result;
            }
        }
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if (m_owners[seat] == m_local) {
                result.push_back(*frame->second[seat]);
            }
        }
    }
    return result;
}

u64 RollbackInputs::confirmedTick() const {
    const u64 executed = m_next - 1;
    u64 held = std::numeric_limits<u64>::max();
    for (u8 peer = 2; peer <= 4; ++peer) {
        // A dropped peer's earlier input is all the host's, as its drop says.
        if (m_hostReceipts[peer] && m_suspendedFrom[peer] == 0) {
            held = std::min(held, *m_hostReceipts[peer]);
        }
    }
    return std::min({m_actualThrough, executed, m_mismatch.value_or(m_next) - 1, held});
}

bool RollbackInputs::observeHostReceipt(u8 peer, u64 through) {
    if (!m_open || m_local == 1 || peer == 1 || peer == m_local || !occupied(peer)) {
        return false;
    }
    m_hostReceipts[peer] = std::max(m_hostReceipts[peer].value_or(0), through);
    return true;
}

bool RollbackInputs::ready() const {
    if (!m_open || m_mismatch || m_next == std::numeric_limits<u64>::max() ||
        m_next - confirmedTick() > kPredictionTicks) {
        return false;
    }
    const auto next = m_actual.find(m_next);
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        if (m_owners[seat] == m_local && (next == m_actual.end() || !next->second[seat])) {
            return false;
        }
    }
    return true;
}

InputCommand RollbackInputs::prediction(u8 seat, u64 tick) const {
    std::optional<InputCommand> previous = m_predictionSeed[seat];
    auto frame = m_actual.lower_bound(tick);
    while (frame != m_actual.begin()) {
        --frame;
        if (frame->second[seat]) {
            previous = frame->second[seat];
            break;
        }
    }
    InputCommand command = previous.value_or(InputCommand{});
    command.epoch = m_epoch;
    command.tick = tick;
    command.grant = m_grants[seat];
    command.seat = seat;
    command.pressedButtons = 0;
    command.hintTopic = command.hintRevision = 0;
    command.console.reset();
    return command;
}

std::optional<RollbackInputs::Frame> RollbackInputs::advance() {
    if (!ready()) {
        return std::nullopt;
    }
    Frame frame;
    auto actual = m_actual.find(m_next);
    // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
    for (u8 seat = 0; seat < static_cast<u8>(m_owners.size()); ++seat) {
        if (m_owners[seat] == 0) {
            continue;
        }
        if ((actual == m_actual.end() || !actual->second[seat]) && suspendedSeat(seat, m_next)) {
            actual = m_actual.try_emplace(m_next).first;
            actual->second[seat] = neutral(seat, m_next);
        }
        if (actual != m_actual.end() && actual->second[seat]) {
            frame.commands[seat] = *actual->second[seat];
        } else {
            frame.commands[seat] = prediction(seat, m_next);
            frame.predictedSeats |= static_cast<u8>(1U << seat);
        }
    }
    m_used.insert_or_assign(m_next, frame);
    ++m_next;
    trim();
    return frame;
}

bool RollbackInputs::rewind(u64 tick) {
    if (!m_open || !m_mismatch || tick != *m_mismatch || tick >= m_next || !m_used.contains(tick)) {
        return false;
    }
    m_replayEnd = std::max(m_replayEnd, m_next);
    m_replayStart = tick;
    m_next = tick;
    m_used.erase(m_used.lower_bound(tick), m_used.end());
    m_mismatch.reset();
    return true;
}

bool RollbackInputs::finishReplayAt(u64 nextTick) {
    if (!m_open || !replaying() || m_mismatch || nextTick != m_next || nextTick <= m_replayStart ||
        nextTick <= confirmedTick()) {
        return false;
    }
    m_used.erase(m_used.lower_bound(nextTick), m_used.end());
    m_replayEnd = nextTick;
    return true;
}

void RollbackInputs::trim() {
    if (m_next <= kHistoryTicks) {
        return;
    }
    const u64 oldest = m_next - kHistoryTicks;
    auto frame = m_actual.begin();
    while (frame != m_actual.end() && frame->first < oldest) {
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if (frame->second[seat]) {
                m_predictionSeed[seat] = frame->second[seat];
            }
        }
        frame = m_actual.erase(frame);
    }
    m_used.erase(m_used.begin(), m_used.lower_bound(oldest));
}

RollbackInputs::Admission RollbackInputs::observePeerTick(u8 peer, u64 epoch, u64 nextTick,
                                                          u8 transitTicks) {
    if (!m_open || !occupied(peer) || peer == m_local) {
        return Admission::WrongPeer;
    }
    if (epoch != m_epoch) {
        return Admission::WrongEpoch;
    }
    if (nextTick == 0 || transitTicks > kPredictionTicks) {
        return Admission::Invalid;
    }
    if (nextTick >= m_next && nextTick - m_next > kMaxAhead) {
        return Admission::TooEarly;
    }
    if (nextTick < m_peerTicks[peer]) {
        return Admission::Stale;
    }
    m_peerTransitTicks[peer] = transitTicks;
    if (nextTick == m_peerTicks[peer]) {
        return Admission::Duplicate;
    }
    m_peerTicks[peer] = nextTick;
    return Admission::Accepted;
}

std::optional<RollbackInputs::Commands> RollbackInputs::actualFrame(u64 tick) const {
    const auto frame = m_actual.find(tick);
    if (!m_open || tick == 0 ||
        !complete(tick, frame == m_actual.end() ? Actual{} : frame->second)) {
        return std::nullopt;
    }
    Commands result{};
    for (usize seat = 0; seat < result.size(); ++seat) {
        if (frame != m_actual.end() && frame->second[seat]) {
            result[seat] = *frame->second[seat];
        } else if (suspendedSeat(seat, tick)) {
            result[seat] = neutral(static_cast<u8>(seat), tick);
        }
    }
    return result;
}

std::vector<InputCommand> RollbackInputs::actualAfter(u64 tick) const {
    std::vector<InputCommand> result;
    if (!m_open) {
        return result;
    }
    for (auto frame = m_actual.upper_bound(tick); frame != m_actual.end(); ++frame) {
        for (const auto& command : frame->second) {
            if (command) {
                result.push_back(*command);
            }
        }
    }
    return result;
}

bool RollbackInputs::adoptActual(const InputCommand& command) {
    if (!m_open || !command.valid() || command.tick == 0 || command.epoch != m_epoch ||
        command.seat >= m_owners.size() || m_owners[command.seat] == 0 ||
        command.grant != m_grants[command.seat] ||
        (command.console &&
         (m_owners[command.seat] != 1 || m_owners[command.console->seat] == 0))) {
        return false;
    }
    if (suspendedSeat(command.seat, command.tick)) {
        return true; // still from its drop on, as live input from there is ignored
    }
    auto& slot = m_actual[command.tick][command.seat];
    if (slot) {
        return same(*slot, command);
    }
    slot = command;
    const auto used = m_used.find(command.tick);
    if (used != m_used.end() && !same(command, used->second.commands[command.seat])) {
        m_mismatch = m_mismatch ? std::min(*m_mismatch, command.tick) : command.tick;
    }
    while (m_actualThrough != std::numeric_limits<u64>::max()) {
        const auto next = m_actual.find(m_actualThrough + 1);
        if (next == m_actual.end() ? !complete(m_actualThrough + 1, Actual{})
                                   : !complete(m_actualThrough + 1, next->second)) {
            break;
        }
        ++m_actualThrough;
    }
    return true;
}

bool RollbackInputs::resumeSampling() {
    if (!m_open || replaying() || m_mismatch) {
        return false;
    }
    u64 last = m_next - 1;
    for (auto frame = m_actual.lower_bound(m_next); frame != m_actual.end(); ++frame) {
        bool local = frame->first == last + 1;
        for (usize seat = 0; seat < m_owners.size() && local; ++seat) {
            local = m_owners[seat] != m_local || frame->second[seat].has_value();
        }
        if (!local) {
            break;
        }
        last = frame->first;
    }
    m_sample = last + 1;
    m_redundancy.clear();
    for (u64 tick = last >= kRedundantTicks ? last - kRedundantTicks + 1 : 1; tick <= last;
         ++tick) {
        const auto frame = m_actual.find(tick);
        if (frame == m_actual.end()) {
            continue;
        }
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if (m_owners[seat] == m_local && frame->second[seat]) {
                m_redundancy.push_back(*frame->second[seat]);
            }
        }
    }
    return true;
}

bool RollbackInputs::timeSyncIdle() const {
    if (!m_open || replaying()) {
        return false;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (peer != m_local && occupied(peer) && m_suspendedFrom[peer] == 0 &&
            m_next > m_peerTicks[peer] &&
            m_next - m_peerTicks[peer] > kPeerSkew + m_peerTransitTicks[peer]) {
            return true;
        }
    }
    return false;
}

} // namespace gdl::game
