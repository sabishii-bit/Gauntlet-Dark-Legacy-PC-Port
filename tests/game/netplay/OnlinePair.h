#pragma once

#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/OnlineSession.h"

namespace gdl::test {
/** In-memory authenticated transport for native gameplay tests. The separate
 * process harness covers the same OnlineSession with real HTTP and ICE. */
struct OnlinePair {
    struct Service final : game::RoomService {
        game::RoomSnapshot& room;
        std::string peer;
        bool closed = false;
        Service(game::RoomSnapshot& snapshot, std::string identity)
            : room(snapshot), peer(std::move(identity)) {}
        Update poll() override { return {room, peer, {}, closed}; }
        bool send(PeerTransport::Signal /*signal*/) override { return true; }
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
        bool configurePeer(std::string peer, bool isHost) override {
            identity = std::move(peer);
            host = isHost;
            return true;
        }
        bool authorizePeers(std::span<const std::string> peers) override {
            REQUIRE(peers.size() == 1);
            remoteIdentity = peers.front();
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
        std::vector<Signal> takeSignals() override { return {}; }
        bool receiveSignal(const Signal& /*signal*/) override { return true; }
        SendResult send(Connection connection, std::span<const u8> bytes,
                        Delivery /*delivery*/) override {
            REQUIRE(connection == (host ? 1U : 99U));
            remote->events.push_back(
                {EventType::Message, host ? 99U : 1U, {bytes.begin(), bytes.end()}, {}});
            return SendResult::Sent;
        }
        std::vector<Event> poll() override { return std::exchange(events, {}); }
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
        {{std::string(32, 'a'), {0}, false}, {std::string(32, 'b'), {1}, false}},
        {}};
    Service hostService{room, std::string(32, 'a')};
    Service guestService{room, std::string(32, 'b')};
    Wire hostWire;
    Wire guestWire;
    game::OnlineSession hostSession;
    game::OnlineSession guestSession;
    game::MatchSession& host = hostSession.match();
    game::MatchSession& guest = guestSession.match();

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
        pump();
        pump(); // Party relay and acknowledgement precede the scene-load barrier.
        REQUIRE(host.phase() == game::MatchSession::Phase::Lobby);
        REQUIRE(guest.phase() == game::MatchSession::Phase::Lobby);
    }
    void pump() {
        for (s32 i = 0; i < 3; ++i) {
            hostSession.update(0);
            guestSession.update(0);
            REQUIRE(hostSession.phase() != game::OnlineSession::Phase::Failed);
            REQUIRE(guestSession.phase() != game::OnlineSession::Phase::Failed);
        }
    }
    void prepare(game::MatchTransition transition = game::MatchTransition::Start) {
        REQUIRE(host.prepare(1, transition));
        pump();
    }
    void ready() {
        REQUIRE(host.loaded());
        REQUIRE(guest.loaded());
        pump();
        REQUIRE(host.phase() == game::MatchSession::Phase::Running);
        REQUIRE(guest.phase() == game::MatchSession::Phase::Running);
    }
};
} // namespace gdl::test
