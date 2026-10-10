#include "game/netplay/MatchSession.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLT");
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
bool validOwners(const MatchOwners& owners) {
    return std::ranges::all_of(owners, [](u8 peer) { return peer <= 4; }) &&
           std::ranges::find(owners, 1) != owners.end();
}
bool belongs(const MatchOwners& owners, u8 peer) {
    return peer != 0 && std::ranges::find(owners, peer) != owners.end();
}
u8 seatsOf(const MatchOwners& owners, u8 peer = 0) {
    u8 mask = 0;
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if (owners[seat] != 0 && (peer == 0 || owners[seat] == peer)) {
            mask |= static_cast<u8>(1U << seat);
        }
    }
    return mask;
}
} // namespace

bool MatchContext::valid() const {
    if (epoch == 0 || scene == 0 || scene > 65535 || tickRate != 60 || inputLead > 12 ||
        static_cast<u8>(transition) > 2 || !validOwners(owners)) {
        return false;
    }
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if ((owners[seat] != 0) != (grants[seat] != 0)) {
            return false;
        }
    }
    return true;
}
bool MatchControl::valid() const {
    const bool vote = kind == MatchControlKind::MovieSkipVote;
    return context.valid() && static_cast<u8>(kind) <= 6 && static_cast<u8>(stop) <= 4 &&
           ((kind == MatchControlKind::Stop) == (stop != MatchStop::None)) &&
           (vote == (movieSkipVotes != 0)) && (movieSkipVotes & ~seatsOf(context.owners)) == 0 &&
           (!vote || context.transition != MatchTransition::Resume);
}
std::optional<std::vector<u8>> MatchControlPacket::encode(const MatchControl& control) {
    if (!control.valid()) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    bytes.reserve(kBytes);
    word(bytes, kMagic);
    bytes.insert(bytes.end(), {3, static_cast<u8>(control.kind), static_cast<u8>(control.stop),
                               control.movieSkipVotes});
    const auto& context = control.context;
    word(bytes, static_cast<u32>(context.epoch));
    word(bytes, static_cast<u32>(context.epoch >> 32U));
    word(bytes, context.scene);
    bytes.insert(bytes.end(),
                 {static_cast<u8>(context.tickRate), static_cast<u8>(context.tickRate >> 8U),
                  context.inputLead, static_cast<u8>(context.transition)});
    bytes.insert(bytes.end(), context.owners.begin(), context.owners.end());
    for (const auto grant : context.grants) {
        word(bytes, grant);
    }
    return bytes;
}
std::optional<MatchControl> MatchControlPacket::decode(std::span<const u8> bytes) {
    if (bytes.size() != kBytes) {
        return std::nullopt;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic || reader.readU8() != 3) {
        return std::nullopt;
    }
    MatchControl control;
    control.kind = static_cast<MatchControlKind>(reader.readU8());
    control.stop = static_cast<MatchStop>(reader.readU8());
    control.movieSkipVotes = reader.readU8();
    auto& context = control.context;
    context.epoch = reader.readU32();
    context.epoch |= static_cast<u64>(reader.readU32()) << 32U;
    context.scene = reader.readU32();
    context.tickRate = reader.readU16();
    context.inputLead = reader.readU8();
    context.transition = static_cast<MatchTransition>(reader.readU8());
    for (auto& owner : context.owners) {
        owner = reader.readU8();
    }
    for (auto& grant : context.grants) {
        grant = reader.readU32();
    }
    return control.valid() ? std::optional{control} : std::nullopt;
}

bool MatchSession::open(u8 localPeer, const MatchOwners& owners, std::span<const MatchLink> links) {
    if (m_phase != Phase::Offline || !validOwners(owners) || !belongs(owners, localPeer)) {
        return false;
    }
    std::array<PacketTransport::Connection, 5> connections{};
    for (const auto& link : links) {
        if (!belongs(owners, link.peer) || link.peer == localPeer || link.connection == 0 ||
            (localPeer != 1 && link.peer != 1) || connections[link.peer] != 0 ||
            std::ranges::find(connections, link.connection) != connections.end()) {
            return false;
        }
        connections[link.peer] = link.connection;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (peer != localPeer && belongs(owners, peer) && (localPeer == 1 || peer == 1) &&
            connections[peer] == 0) {
            return false;
        }
    }
    m_local = localPeer;
    m_owners = owners;
    m_links = connections;
    for (usize seat = 0; seat < owners.size(); ++seat) {
        if (owners[seat] != 0) {
            m_timeline.assign(seat, owners[seat]);
        }
    }
    m_phase = Phase::Lobby;
    return true;
}
void MatchSession::clear() {
    *this = MatchSession{};
}
u8 MatchSession::peerOf(PacketTransport::Connection connection) const {
    if (connection != 0) {
        for (u8 peer = 1; peer <= 4; ++peer) {
            if (m_links[peer] == connection) {
                return peer;
            }
        }
    }
    return 0;
}
bool MatchSession::queue(u8 peer, MatchControlKind kind, MatchStop reason, u8 votes) {
    const auto bytes = MatchControlPacket::encode({kind, reason, m_context, votes});
    if (peer == 0 || peer > 4 || m_links[peer] == 0 || !bytes ||
        m_controls.size() >= kControlBudget) {
        return false;
    }
    m_controls.push_back({m_links[peer], *bytes});
    return true;
}
bool MatchSession::broadcast(MatchControlKind kind, MatchStop reason, u8 votes) {
    const auto peers =
        std::ranges::count_if(m_links, [](auto connection) { return connection != 0; });
    if (m_controls.size() + static_cast<usize>(peers) > kControlBudget) {
        return false;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0 && !queue(peer, kind, reason, votes)) {
            return false;
        }
    }
    return true;
}
void MatchSession::discardGameplay() {
    m_history.clear();
    m_pendingPress = {};
    m_input.clear();
    m_states = {};
    m_published.reset();
}
bool MatchSession::prepare(u32 scene, MatchTransition transition, const MatchParty* party) {
    const bool allowed = (transition == MatchTransition::Start && m_phase == Phase::Lobby) ||
                         (transition == MatchTransition::Travel && m_phase == Phase::Running) ||
                         (transition == MatchTransition::Resume && m_phase == Phase::Paused &&
                          scene == m_context.scene);
    if (!host() || !allowed || scene == 0 || scene > 65535 ||
        (transition == MatchTransition::Travel) != (party != nullptr) ||
        m_timeline.epoch() == std::numeric_limits<u64>::max()) {
        return false;
    }
    std::vector<std::vector<u8>> profiles;
    if (party != nullptr) {
        for (usize seat = 0; seat < m_owners.size(); ++seat) {
            if ((*party)[seat].has_value() != (m_owners[seat] != 0)) {
                return false;
            }
            if ((*party)[seat]) {
                auto packet = PartyCheckpointPacket::encode(
                    {m_timeline.epoch() + 1, static_cast<u8>(seat), *(*party)[seat]});
                if (!packet) {
                    return false;
                }
                profiles.push_back(std::move(*packet));
            }
        }
    }
    const auto peers = static_cast<usize>(
        std::ranges::count_if(m_links, [](auto connection) { return connection != 0; }));
    if (m_controls.size() + peers * (1 + profiles.size()) > kControlBudget) {
        return false;
    }
    // Copy before replacing internal storage: callers may reuse travelParty().
    MatchParty nextParty = party != nullptr ? *party : MatchParty{};
    m_timeline.beginEpoch();
    m_context = {};
    m_context.epoch = m_timeline.epoch();
    m_context.scene = scene;
    m_context.transition = transition;
    m_context.owners = m_owners;
    m_travelParty = std::move(nextParty);
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        m_context.grants[seat] = m_owners[seat] == 0 ? 0 : m_timeline.grant(seat);
    }
    discardGameplay();
    m_loaded = {};
    m_movieSkipVotes = 0;
    m_movieSkipRequests = 0;
    m_loadingSeconds = 0;
    m_phase = Phase::Loading;
    if (!broadcast(MatchControlKind::Prepare)) {
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    for (const auto connection : m_links) {
        if (connection != 0) {
            for (const auto& profile : profiles) {
                m_controls.push_back({connection, profile});
            }
        }
    }
    return true;
}
const MatchParty* MatchSession::travelParty() const {
    if (m_context.transition != MatchTransition::Travel ||
        (m_phase != Phase::Loading && m_phase != Phase::Running && m_phase != Phase::Paused)) {
        return nullptr;
    }
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        if (m_travelParty[seat].has_value() != (m_owners[seat] != 0)) {
            return nullptr;
        }
    }
    return &m_travelParty;
}
MatchSession::Admission MatchSession::checkpoint(u8 peer, const PartyCheckpoint& message) {
    if (host() || peer != 1) {
        return Admission::WrongPeer;
    }
    if (message.epoch < m_context.epoch) {
        return Admission::Stale;
    }
    if (message.epoch != m_context.epoch || m_context.transition != MatchTransition::Travel ||
        m_owners[message.seat] == 0) {
        return Admission::Invalid;
    }
    auto& profile = m_travelParty[message.seat];
    if (profile) {
        return CharacterProfilePacket::encode(*profile) ==
                       CharacterProfilePacket::encode(message.profile)
                   ? Admission::Accepted
                   : Admission::Invalid;
    }
    if (m_phase != Phase::Loading || m_loaded[m_local]) {
        return Admission::WrongPhase;
    }
    profile = message.profile;
    return Admission::Accepted;
}
void MatchSession::tryCommit() {
    if (!host() || m_phase != Phase::Loading) {
        return;
    }
    for (const auto owner : m_owners) {
        if (owner != 0 && !m_loaded[owner]) {
            return;
        }
    }
    if (!broadcast(MatchControlKind::Commit)) {
        stop(MatchStop::TransportFailure, true);
        return;
    }
    m_phase = Phase::Running;
}
bool MatchSession::loaded() {
    if (m_phase != Phase::Loading || m_loaded[m_local] ||
        (m_context.transition == MatchTransition::Travel && travelParty() == nullptr)) {
        return false;
    }
    m_loaded[m_local] = true;
    if (host()) {
        tryCommit();
    } else if (!queue(1, MatchControlKind::Ready)) {
        stop(MatchStop::TransportFailure, false);
        return false;
    }
    return true;
}
bool MatchSession::requestMovieSkip(u8 seat) {
    if (m_phase != Phase::Loading || m_context.transition == MatchTransition::Resume ||
        seat >= m_owners.size() || m_owners[seat] != m_local) {
        return false;
    }
    const auto bit = static_cast<u8>(1U << seat);
    if ((m_movieSkipRequests & bit) != 0) {
        return true;
    }
    const auto votes = static_cast<u8>(m_movieSkipVotes | bit);
    if (host() ? !broadcast(MatchControlKind::MovieSkipVote, MatchStop::None, votes)
               : !queue(1, MatchControlKind::MovieSkipVote, MatchStop::None, bit)) {
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    m_movieSkipRequests |= bit;
    if (host()) {
        m_movieSkipVotes = votes;
    }
    return true;
}
bool MatchSession::movieSkipReady() const {
    const auto required = seatsOf(m_owners);
    return m_phase == Phase::Loading && m_context.transition != MatchTransition::Resume &&
           required != 0 && m_movieSkipVotes == required;
}
bool MatchSession::requestPause() {
    if (m_phase != Phase::Running) {
        return false;
    }
    if (!host()) {
        // One outstanding request per epoch is enough; keep its reliable delivery.
        if (std::ranges::any_of(m_controls, [](const auto& pending) {
                const auto control = MatchControlPacket::decode(pending.bytes);
                return control && control->kind == MatchControlKind::RequestPause;
            })) {
            return true;
        }
        return queue(1, MatchControlKind::RequestPause);
    }
    discardGameplay();
    m_phase = Phase::Paused;
    if (!broadcast(MatchControlKind::Pause)) {
        stop(MatchStop::TransportFailure, true);
        return false;
    }
    return true;
}
void MatchSession::stop(MatchStop reason, bool notify) {
    if (m_phase == Phase::Offline || m_phase == Phase::Stopped) {
        return;
    }
    discardGameplay();
    m_controls.clear();
    m_playback.clear();
    m_phase = Phase::Stopped;
    m_stop = reason;
    if (notify && m_context.valid()) {
        if (host()) {
            broadcast(MatchControlKind::Stop, reason);
        } else {
            queue(1, MatchControlKind::Stop, reason);
        }
    }
}
void MatchSession::leave() {
    stop(MatchStop::Left, true);
}
void MatchSession::disconnected(PacketTransport::Connection connection) {
    const auto peer = peerOf(connection);
    if (peer == 0) {
        return;
    }
    m_links[peer] = 0;
    m_timeline.disconnect(peer);
    std::erase_if(m_controls, [&](const auto& queued) { return queued.connection == connection; });
    m_states[peer] = {};
    stop(MatchStop::Disconnected, host());
}
void MatchSession::update(f64 seconds) {
    if (m_phase == Phase::Loading && std::isfinite(seconds) && seconds > 0) {
        m_loadingSeconds += seconds;
        if (m_loadingSeconds >= kLoadTimeout) {
            stop(MatchStop::LoadTimeout, true);
        }
    }
}
MatchSession::Admission MatchSession::control(u8 peer, const MatchControl& message) {
    // A host can abort before its queued Prepare reaches a guest. Stop is final,
    // not a request to start that future epoch or load its scene.
    if (!host() && peer == 1 && message.kind == MatchControlKind::Stop &&
        message.context.owners == m_owners && message.context.epoch >= m_context.epoch) {
        stop(message.stop, false);
        return Admission::Accepted;
    }
    if (message.kind == MatchControlKind::Prepare) {
        if (host() || peer != 1) {
            return Admission::WrongPeer;
        }
        if (message.context.epoch <= m_context.epoch) {
            return Admission::Stale;
        }
        if (message.context.owners != m_owners) {
            return Admission::Invalid;
        }
        const auto transition = message.context.transition;
        const bool allowed = (transition == MatchTransition::Start && m_phase == Phase::Lobby) ||
                             (transition == MatchTransition::Travel && m_phase == Phase::Running) ||
                             (transition == MatchTransition::Resume && m_phase == Phase::Paused &&
                              message.context.scene == m_context.scene);
        if (!allowed) {
            return Admission::WrongPhase;
        }
        m_context = message.context;
        m_travelParty = {};
        discardGameplay();
        m_loaded = {};
        m_movieSkipVotes = 0;
        m_movieSkipRequests = 0;
        m_loadingSeconds = 0;
        m_nextInput = m_context.inputLead;
        if (!m_playback.begin(m_links[1], m_context.epoch)) {
            return Admission::Invalid;
        }
        m_phase = Phase::Loading;
        return Admission::Accepted;
    }
    if (message.context != m_context) {
        return Admission::Stale;
    }
    switch (message.kind) {
    case MatchControlKind::MovieSkipVote: {
        if (host() && (message.movieSkipVotes & ~seatsOf(m_owners, peer)) != 0) {
            return Admission::WrongPeer;
        }
        // A late vote may follow natural movie completion and the final Ready.
        if (m_phase != Phase::Loading) {
            return Admission::Stale;
        }
        const auto votes = static_cast<u8>(m_movieSkipVotes | message.movieSkipVotes);
        if (host() && votes != m_movieSkipVotes &&
            !broadcast(MatchControlKind::MovieSkipVote, MatchStop::None, votes)) {
            stop(MatchStop::TransportFailure, true);
            return Admission::Accepted;
        }
        m_movieSkipVotes = votes;
        return Admission::Accepted;
    }
    case MatchControlKind::Ready:
        if (!host()) {
            return Admission::WrongPeer;
        }
        if (m_phase != Phase::Loading) {
            return Admission::WrongPhase;
        }
        m_loaded[peer] = true;
        tryCommit();
        return Admission::Accepted;
    case MatchControlKind::Commit:
        if (host() || peer != 1) {
            return Admission::WrongPeer;
        }
        if (m_phase != Phase::Loading || !m_loaded[m_local]) {
            return Admission::WrongPhase;
        }
        m_phase = Phase::Running;
        return Admission::Accepted;
    case MatchControlKind::Pause:
        if (host() || peer != 1) {
            return Admission::WrongPeer;
        }
        if (m_phase != Phase::Running) {
            return Admission::WrongPhase;
        }
        discardGameplay();
        m_phase = Phase::Paused;
        return Admission::Accepted;
    case MatchControlKind::RequestPause:
        if (!host()) {
            return Admission::WrongPeer;
        }
        // Different guests can request the same pause before either receives the
        // host's Pause. Their independent reliable channels do not order together.
        if (m_phase == Phase::Paused) {
            return Admission::Stale;
        }
        return requestPause() ? Admission::Accepted : Admission::WrongPhase;
    case MatchControlKind::Stop:
        stop(host() ? MatchStop::Left : message.stop, host());
        return Admission::Accepted;
    case MatchControlKind::Prepare: break;
    }
    return Admission::Invalid;
}
MatchSession::Admission MatchSession::receive(PacketTransport::Connection connection,
                                              std::span<const u8> bytes) {
    const auto peer = peerOf(connection);
    if (peer == 0) {
        return Admission::WrongPeer;
    }
    if (m_phase == Phase::Stopped || m_phase == Phase::Offline) {
        return Admission::WrongPhase;
    }
    if (bytes.size() >= 4 && ByteReader(bytes).readU32() == kMagic) {
        const auto message = MatchControlPacket::decode(bytes);
        return message ? control(peer, *message) : Admission::Invalid;
    }
    if (PartyCheckpointPacket::recognizes(bytes)) {
        const auto message = PartyCheckpointPacket::decode(bytes);
        return message ? checkpoint(peer, *message) : Admission::Invalid;
    }
    if (m_phase != Phase::Running) {
        // Reliable control and unreliable gameplay are not ordered together.
        // A snapshot can overtake Commit/Prepare, or old inputs can trail Pause.
        // Discard those datagrams without decoding, queueing or advancing state;
        // only the reliable control above is allowed to cross a phase boundary.
        return Admission::Stale;
    }
    if (host()) {
        const auto commands = InputPacket::decode(bytes);
        if (!commands) {
            return Admission::Invalid;
        }
        // Validate the whole packet before admission, so a spoofed trailing seat
        // cannot leave an earlier command partially applied.
        for (const auto& command : *commands) {
            if (command.epoch != m_context.epoch) {
                return Admission::Stale;
            }
            if (m_context.owners[command.seat] != peer ||
                command.grant != m_context.grants[command.seat]) {
                return Admission::WrongPeer;
            }
            if (command.tick > m_timeline.tick() &&
                command.tick - m_timeline.tick() > InputTimeline::kMaxAhead) {
                return Admission::Invalid;
            }
        }
        for (const auto& command : *commands) {
            m_timeline.submit(peer, command);
        }
        return Admission::Accepted;
    }
    const auto admission = m_playback.receive(connection, bytes);
    switch (admission) {
    case CombatReplica::Admission::Committed:
        if (const auto* state = m_playback.latest();
            state != nullptr &&
            state->motion.tick <= std::numeric_limits<u64>::max() - m_context.inputLead) {
            m_nextInput = std::max(m_nextInput, state->motion.tick + m_context.inputLead);
        }
        return Admission::Accepted;
    case CombatReplica::Admission::Pending:
    case CombatReplica::Admission::Duplicate: return Admission::Accepted;
    case CombatReplica::Admission::Stale:
    case CombatReplica::Admission::WrongEpoch: return Admission::Stale;
    case CombatReplica::Admission::WrongHost: return Admission::WrongPeer;
    case CombatReplica::Admission::Invalid: return Admission::Invalid;
    }
    return Admission::Invalid;
}
bool MatchSession::inputReady() const {
    if (m_phase != Phase::Running || inputTick() == std::numeric_limits<u64>::max()) {
        return false;
    }
    if (host()) {
        return true;
    }
    const auto* latest = m_playback.latest();
    const u64 confirmed = latest == nullptr ? 0 : latest->motion.tick;
    return m_nextInput <= confirmed || m_nextInput - confirmed <= m_context.inputLead + kInputSlack;
}
bool MatchSession::sample(std::span<const InputCommand> frame) {
    if (m_phase != Phase::Running || inputTick() == std::numeric_limits<u64>::max() ||
        frame.size() != static_cast<usize>(std::ranges::count(m_owners, m_local))) {
        return false;
    }
    std::array<bool, InputCommand::kSeats> seen{};
    for (const auto& command : frame) {
        if (!command.valid() || command.epoch != m_context.epoch || command.tick != inputTick() ||
            m_owners[command.seat] != m_local || command.grant != m_context.grants[command.seat] ||
            seen[command.seat]) {
            return false;
        }
        seen[command.seat] = true;
    }
    if (!inputReady()) {
        for (const auto& command : frame) {
            m_pendingPress[command.seat] |= command.pressedButtons;
        }
        return true;
    }
    std::vector<InputCommand> pending(frame.begin(), frame.end());
    for (auto& command : pending) {
        command.pressedButtons |= m_pendingPress[command.seat];
    }
    if (!m_history.record(pending)) {
        return false;
    }
    if (host()) {
        for (const auto& command : frame) {
            m_timeline.submit(m_local, command);
        }
    } else {
        const auto bytes = InputPacket::encode(m_history.commands());
        if (!bytes) {
            return false;
        }
        m_input = *bytes;
        ++m_nextInput;
    }
    m_pendingPress = {};
    return true;
}
std::optional<InputTimeline::Frame> MatchSession::advance() {
    if (!host() || m_phase != Phase::Running ||
        m_timeline.tick() == std::numeric_limits<u64>::max()) {
        return std::nullopt;
    }
    return m_timeline.advance();
}
bool MatchSession::publish(const CombatSnapshot& state) {
    if (!host() || m_phase != Phase::Running || m_timeline.tick() == 0 ||
        state.motion.epoch != m_context.epoch || state.motion.tick != m_timeline.tick() - 1 ||
        (m_published && state.motion.tick <= *m_published)) {
        return false;
    }
    for (usize seat = 0; seat < m_owners.size(); ++seat) {
        const auto& player = state.motion.players[seat];
        if (player.has_value() != (m_owners[seat] != 0) ||
            (player && player->grant != m_context.grants[seat])) {
            return false;
        }
    }
    const auto packets = CombatReplica::packets(state, SnapshotBlock::Compression::Automatic,
                                                CombatReplica::Recovery::SingleLoss);
    if (!packets) {
        return false;
    }
    for (u8 peer = 1; peer <= 4; ++peer) {
        if (m_links[peer] != 0) {
            auto& delivery = m_states[peer];
            if (delivery.next > 0 && delivery.next < delivery.packets.size()) {
                // Finish the fragments already in flight. Replacing a partly sent
                // checkpoint every capture can starve the receiver forever under
                // congestion. Keep only one newer replacement, never a FIFO.
                delivery.latest = *packets;
            } else {
                delivery = {*packets, 0, {}};
            }
        }
    }
    m_published = state.motion.tick;
    return true;
}
void MatchSession::flush(PacketTransport& transport) {
    while (!m_controls.empty()) {
        const auto& control = m_controls.front();
        const auto result =
            transport.send(control.connection, control.bytes, PacketTransport::Delivery::Reliable);
        if (result == PacketTransport::SendResult::Congested) {
            return;
        }
        if (result != PacketTransport::SendResult::Sent) {
            const auto connection = control.connection;
            disconnected(connection);
            return;
        }
        m_controls.pop_front();
    }
    if (m_phase != Phase::Running) {
        return;
    }
    if (!host() && !m_input.empty()) {
        const auto result =
            transport.send(m_links[1], m_input, PacketTransport::Delivery::Unreliable);
        if (result == PacketTransport::SendResult::Sent) {
            m_input.clear();
        } else if (result != PacketTransport::SendResult::Congested) {
            stop(MatchStop::TransportFailure, false);
        }
    }
    for (u8 peer = 1; host() && peer <= 4; ++peer) {
        auto& state = m_states[peer];
        while (true) {
            if (state.next == state.packets.size()) {
                if (state.latest.empty()) {
                    break;
                }
                state.packets = std::move(state.latest);
                state.latest.clear();
                state.next = 0;
            }
            const auto result = transport.send(m_links[peer], state.packets[state.next],
                                               PacketTransport::Delivery::Unreliable);
            if (result == PacketTransport::SendResult::Congested) {
                break;
            }
            if (result != PacketTransport::SendResult::Sent) {
                disconnected(m_links[peer]);
                return;
            }
            ++state.next;
        }
    }
}
} // namespace gdl::game
