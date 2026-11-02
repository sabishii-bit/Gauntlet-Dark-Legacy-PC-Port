#pragma once

#include <functional>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/OnlineSession.h"
#include "game/online/NativePlaySetup.h"
#include "game/rollback/RollbackSessionHarness.h"
#include "game/rollback/SessionInputs.h"

namespace gdl::test {
/** In-memory authenticated transport for native gameplay tests. The separate
 * process harness covers the same OnlineSession over the real provider adapter. */
struct OnlinePair {
    struct Service final : game::RoomService {
        game::RoomSnapshot& room;
        std::string peer;
        bool closed = false;
        Service(game::RoomSnapshot& snapshot, std::string identity)
            : room(snapshot), peer(std::move(identity)) {}
        Update poll() override { return {room, peer, {}, closed}; }
        bool ready(u64 revision, bool value) override {
            if (revision != room.revision || room.started) {
                return false;
            }
            for (auto& member : room.members) {
                if (member.peer == peer) {
                    member.ready = value;
                }
            }
            return true;
        }
        bool start(u64 revision) override {
            if (peer != room.host || revision != room.revision || room.started ||
                !room.members[0].ready || !room.members[1].ready) {
                return false;
            }
            room.started = true;
            return true;
        }
        void leave() override { closed = true; }
    };
    struct Wire final : PeerTransport {
        Wire* remote = nullptr;
        bool host = false;
        std::string identity;
        std::string remoteIdentity;
        std::vector<Event> events;
        struct Delayed {
            u64 due;
            Event event;
        };
        std::vector<Delayed> delayed;
        u64 polls = 0;
        u64 latency = 0;     ///< receiver polls; zero preserves the immediate harness
        bool silent = false; ///< sends succeed but reach nobody: a peer gone quiet
        std::function<SendResult(std::span<const u8>, Delivery)> beforeSend;
        bool configurePeer(std::string peer, bool isHost) override {
            identity = std::move(peer);
            host = isHost;
            return true;
        }
        bool authorizePeers(std::span<const std::string> peers) override {
            REQUIRE(peers.size() <= 1); // the other machine, or nobody once it is dropped
            if (!peers.empty()) {
                remoteIdentity = peers.front();
            }
            return true;
        }
        std::optional<Connection> connectPeer(const std::string& peer) override {
            REQUIRE_FALSE(host);
            REQUIRE(peer == remoteIdentity);
            events.push_back({EventType::Connected, 99, {}, {}});
            remote->events.push_back({EventType::Connected, 1, {}, {}});
            return 99;
        }
        std::optional<std::string> peer(Connection connection) const override {
            return connection == (host ? 1U : 99U) ? std::optional{remoteIdentity} : std::nullopt;
        }
        SendResult send(Connection connection, std::span<const u8> bytes,
                        Delivery delivery) override {
            REQUIRE(connection == (host ? 1U : 99U));
            if (beforeSend) {
                const auto result = beforeSend(bytes, delivery);
                if (result != SendResult::Sent) {
                    return result;
                }
            }
            Event event{EventType::Message, host ? 99U : 1U, {bytes.begin(), bytes.end()}, {}};
            if (silent) {
                return SendResult::Sent;
            }
            if (latency == 0) {
                remote->events.push_back(std::move(event));
            } else {
                remote->delayed.push_back({remote->polls + latency, std::move(event)});
            }
            return SendResult::Sent;
        }
        std::vector<Event> poll() override {
            ++polls;
            for (auto& delivery : delayed) {
                if (delivery.due <= polls) {
                    events.push_back(std::move(delivery.event));
                }
            }
            std::erase_if(delayed, [this](const auto& delivery) { return delivery.due <= polls; });
            return std::exchange(events, {});
        }
        std::optional<Statistics> statistics(Connection /*connection*/) const override {
            return {};
        }
        void close(Connection /*connection*/) override {}
    };

    game::RoomSnapshot room{
        "ABCDEFGH",
        std::string(32, 'a'),
        1,
        false,
        {{std::string(32, 'a'), {0}, false}, {std::string(32, 'b'), {1}, false}}};
    Service hostService{room, std::string(32, 'a')};
    Service guestService{room, std::string(32, 'b')};
    Wire hostWire;
    Wire guestWire;
    game::OnlineSession hostSession;
    game::OnlineSession guestSession;
    game::MatchSession& host = hostSession.match();
    game::MatchSession& guest = guestSession.match();
    RollbackSessionHarness hostInputs, guestInputs;

    explicit OnlinePair(std::optional<std::array<game::CharacterProfile, 2>> profiles = {},
                        bool autoStart = true, u8 hostPlayers = 1, u8 guestPlayers = 1)
        : hostSession(hostService, hostWire, true, hostPlayers),
          guestSession(guestService, guestWire, false, guestPlayers) {
        REQUIRE(hostPlayers > 0);
        REQUIRE(guestPlayers > 0);
        REQUIRE(hostPlayers + guestPlayers <= 4);
        room.members[0].seats.clear();
        room.members[1].seats.clear();
        for (u8 seat = 0; seat < hostPlayers + guestPlayers; ++seat) {
            room.members[seat < hostPlayers ? 0 : 1].seats.push_back(seat);
        }
        hostWire.remote = &guestWire;
        guestWire.remote = &hostWire;
        const game::OnlineSession::NativeIdentity identity{
            "test-executable", "test-runtime", std::string(64, 'a'), std::string(64, 'b'), "en"};
        game::OnlineSession::NativeRules rules;
        rules.scene = 1;
        REQUIRE(hostSession.configureNative(identity, rules));
        REQUIRE(guestSession.configureNative(identity));
        game::CharacterProfile selection;
        selection.name = "HOST";
        REQUIRE(hostSession.select(std::vector<game::CharacterProfile>(
            hostPlayers, profiles ? (*profiles)[0] : selection)));
        selection.name = "GUEST";
        selection.character = 1;
        selection.color = 1;
        REQUIRE(guestSession.select(std::vector<game::CharacterProfile>(
            guestPlayers, profiles ? (*profiles)[1] : selection)));
        pump();
        if (autoStart) {
            start();
        }
    }
    void start() {
        REQUIRE(hostSession.ready());
        REQUIRE(guestSession.ready());
        pump();
        REQUIRE(hostSession.start());
        for (u32 round = 0;
             round < 12 && guestSession.phase() != game::OnlineSession::Phase::Active; ++round) {
            pump();
        }
        REQUIRE(host.phase() == game::MatchSession::Phase::Lobby);
        REQUIRE(guest.phase() == game::MatchSession::Phase::Lobby);
    }
    void pump() {
        for (s32 i = 0; i < 3; ++i) {
            hostSession.update(0);
            guestSession.update(0);
            for (auto* session : {&hostSession, &guestSession}) {
                if (session->nativeSetupPending()) {
                    const auto& agreed = *session->nativeContract();
                    const game::NativePlaySetup::Contract contract{agreed.rules.scene,
                                                                   agreed.rules.autoMelee,
                                                                   agreed.rules.autoActivateItems,
                                                                   agreed.rules.expandedShop,
                                                                   agreed.rules.autoAim,
                                                                   agreed.identity.build,
                                                                   agreed.identity.content,
                                                                   agreed.identity.data};
                    const auto setup =
                        game::NativePlaySetup::create(game::GameConfig{}, session->room()->settings,
                                                      *session->party(), {}, contract);
                    REQUIRE(setup);
                    REQUIRE(session->confirmNativeSetup(setup->digest()));
                }
            }
            REQUIRE(hostSession.phase() != game::OnlineSession::Phase::Failed);
            REQUIRE(guestSession.phase() != game::OnlineSession::Phase::Failed);
        }
    }
    void prepare(game::MatchTransition transition = game::MatchTransition::Start) {
        REQUIRE(host.prepare(1, transition));
        pump();
    }
    void ready() {
        if (host.context().transition != game::MatchTransition::Shop) {
            REQUIRE(hostInputs.bind(host));
            REQUIRE(guestInputs.bind(guest));
        } else {
            hostInputs.clear();
            guestInputs.clear();
        }
        REQUIRE(host.loaded());
        REQUIRE(guest.loaded());
        pump();
        REQUIRE(host.phase() == game::MatchSession::Phase::Running);
        REQUIRE(guest.phase() == game::MatchSession::Phase::Running);
    }
    RollbackSessionHarness& inputs(game::MatchSession& session) {
        REQUIRE((&session == &host || &session == &guest));
        return &session == &host ? hostInputs : guestInputs;
    }
    bool sample(game::MatchSession& session, const game::SessionInputs::Frame& local) {
        auto& timeline = inputs(session);
        std::vector<game::InputCommand> commands;
        const auto& context = session.context();
        // NOLINTNEXTLINE(bugprone-too-small-loop-variable): seats and peers number at most five
        for (u8 seat = 0; seat < static_cast<u8>(context.owners.size()); ++seat) {
            if (context.owners[seat] == session.localPeer()) {
                commands.push_back(game::SessionInputs::command(
                    local[commands.size()], context.epoch, timeline.sampleTick(),
                    context.grants[seat], seat));
            }
        }
        return timeline.sample(commands);
    }
    std::optional<game::SessionInputs::Frame> advance(game::MatchSession& session) {
        const auto command = inputs(session).advance();
        if (!command) {
            return std::nullopt;
        }
        game::SessionInputs::Frame result{};
        for (usize seat = 0; seat < result.size(); ++seat) {
            result[seat] = game::SessionInputs::playInput(command->commands[seat]);
        }
        return result;
    }
};
} // namespace gdl::test
