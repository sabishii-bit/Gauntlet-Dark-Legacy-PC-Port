#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/HostedRoom.h"
#include "game/netplay/OnlineSession.h"

namespace {
using namespace gdl;
using namespace gdl::game;
struct Wire final : PacketTransport {
    Wire* remote = nullptr;
    std::vector<Event> incoming;
    bool congested = false;
    SendResult send(Connection id, std::span<const u8> bytes, Delivery /*delivery*/) override {
        if (congested) {
            return SendResult::Congested;
        }
        remote->incoming.push_back({EventType::Message, id, {bytes.begin(), bytes.end()}, {}});
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return std::exchange(incoming, {}); }
    std::optional<Statistics> statistics(Connection /*connection*/) const override { return {}; }
    void close(Connection id) override {
        if (remote) {
            remote->incoming.push_back({EventType::Disconnected, id, {}, {}});
        }
    }
};
struct Pair {
    Wire a, b;
    HostedRoom hostRoom, guestRoom;
    OnlineSession host, guest;
    Pair(u8 hostPlayers = 1, u8 guestPlayers = 1, std::string version = "test",
         std::string content = std::string(64, 'a'))
        : hostRoom(a, true, hostPlayers, "test", std::string(64, 'a')),
          guestRoom(b, false, guestPlayers, std::move(version), std::move(content)),
          host(hostRoom, hostRoom.transport(), true, hostPlayers),
          guest(guestRoom, guestRoom.transport(), false, guestPlayers) {
        a.remote = &b;
        b.remote = &a;
        a.incoming.push_back({PacketTransport::EventType::Connected, 7, {}, {}});
        b.incoming.push_back({PacketTransport::EventType::Connected, 7, {}, {}});
        CharacterProfile profile;
        profile.name = "TEST";
        REQUIRE(host.select(std::vector<CharacterProfile>(hostPlayers, profile)));
        REQUIRE(guest.select(std::vector<CharacterProfile>(guestPlayers, profile)));
    }
    ~Pair() {
        host.leave();
        guest.leave();
        hostRoom.leave();
        guestRoom.leave();
        a.remote = nullptr;
        b.remote = nullptr;
    }
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;
    Pair(Pair&&) = delete;
    Pair& operator=(Pair&&) = delete;
    void pump() {
        for (s32 i = 0; i < 10; ++i) {
            host.update(0);
            guest.update(0);
        }
    }
};
} // namespace
TEST_CASE("old point-aim peers cannot join a directional-input room", "[netplay][hosted-room]") {
    Pair pair;
    pair.guest.update(0);
    bool rewrote = false;
    for (auto& event : pair.a.incoming) {
        if (event.type == PacketTransport::EventType::Message && event.bytes.size() >= 2) {
            event.bytes[1] = 1;
            rewrote = true;
        }
    }
    REQUIRE(rewrote);
    pair.pump();
    CHECK_FALSE(pair.guest.connected());
    REQUIRE(pair.host.room());
    CHECK(pair.host.room()->members.size() == 1);
}

TEST_CASE("self hosted admission starts existing online gameplay without a room server",
          "[netplay][hosted-room]") {
    for (u8 players = 1; players <= 3; ++players) {
        Pair pair(players, 1);
        pair.a.congested = true;
        pair.pump();
        CHECK(pair.guest.phase() == OnlineSession::Phase::Joining);
        pair.a.congested = false;
        pair.pump();
        REQUIRE(pair.host.connected());
        REQUIRE(pair.guest.connected());
        REQUIRE(pair.guest.seats().size() == 1);
        CHECK(pair.guest.seats()[0] == players);
        REQUIRE(pair.host.ready());
        REQUIRE(pair.guest.ready());
        pair.pump();
        REQUIRE(pair.host.start());
        pair.pump();
        REQUIRE(pair.host.phase() == OnlineSession::Phase::Active);
        REQUIRE(pair.guest.phase() == OnlineSession::Phase::Active);
        REQUIRE(pair.host.match().prepare(1));
        pair.pump();
        REQUIRE(pair.guest.match().phase() == MatchSession::Phase::Loading);
        REQUIRE(pair.host.match().loaded());
        REQUIRE(pair.guest.match().loaded());
        pair.pump();
        REQUIRE(pair.guest.match().phase() == MatchSession::Phase::Running);
        pair.guest.leave();
        pair.pump();
        REQUIRE(pair.host.phase() == OnlineSession::Phase::Failed);
    }
}
TEST_CASE("hosted room admission rejects mismatched builds assets and full parties",
          "[netplay][hosted-room]") {
    SECTION("build") {
        Pair pair(1, 1, "different");
        pair.pump();
        CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Version);
        CHECK(pair.host.phase() == OnlineSession::Phase::Lobby);
    }
    SECTION("assets") {
        Pair pair(1, 1, "test", std::string(64, 'b'));
        pair.pump();
        CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Assets);
    }
    SECTION("full") {
        Pair pair(3, 2);
        pair.pump();
        CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Full);
    }
}
TEST_CASE("a hosted lobby survives guest departure and resets readiness",
          "[netplay][hosted-room]") {
    Pair pair;
    pair.pump();
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    pair.guest.leave();
    pair.pump();
    REQUIRE(pair.host.phase() == OnlineSession::Phase::Lobby);
    REQUIRE(pair.host.room()->members.size() == 1);
    CHECK_FALSE(pair.host.room()->members[0].ready);
}
TEST_CASE("room rules are host owned replicated and invalidate every ready vote",
          "[netplay][hosted-room][room-settings]") {
    Pair pair;
    pair.pump();
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    const auto revision = pair.host.room()->revision;
    const RoomSettings changed{3, 2, 1};
    CHECK_FALSE(pair.guestRoom.settings(changed));
    CHECK_FALSE(pair.hostRoom.settings({1, 2, 1})); // already two joined
    CHECK_FALSE(pair.hostRoom.settings({4, 3, 0}));
    REQUIRE(pair.hostRoom.settings(changed));
    CHECK_FALSE(pair.hostRoom.start(revision));
    pair.pump();
    CHECK(pair.host.room()->settings == changed);
    CHECK(pair.guest.room()->settings == changed);
    CHECK(pair.host.room()->revision > revision);
    CHECK_FALSE(pair.host.room()->members[0].ready);
    CHECK_FALSE(pair.host.room()->members[1].ready);
    CHECK_FALSE(pair.host.start());
    REQUIRE(pair.host.ready());
    REQUIRE(pair.guest.ready());
    pair.pump();
    REQUIRE(pair.host.start());
    pair.pump();
    CHECK_FALSE(pair.hostRoom.settings({4, 0, 0}));
    CHECK(pair.guest.room()->settings == changed);
}
TEST_CASE("room capacity is enforced when a guest requests multiple local seats",
          "[netplay][hosted-room][room-settings]") {
    Pair pair(1, 2);
    REQUIRE(pair.hostRoom.settings({2, 1, 0}));
    pair.pump();
    CHECK(pair.guestRoom.failure() == HostedRoom::Failure::Full);
    REQUIRE(pair.host.room());
    CHECK(pair.host.room()->members.size() == 1);
}
