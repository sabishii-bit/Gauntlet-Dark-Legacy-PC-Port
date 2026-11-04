#include <algorithm>
#include <chrono>
#include <source_location>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "engine/net/SessionTransport.h"

#include "game/config/GameConfig.h"
#include "game/netplay/HostedRoom.h"
#include "game/netplay/OnlineSession.h"
#include "game/online/AgreedSetup.h"
#include "game/rollback/RollbackSessionHarness.h"

namespace {
using namespace gdl;
using namespace gdl::game;
template <class Predicate, class Step>
void until(Predicate done, Step step, std::chrono::seconds patience = std::chrono::seconds(10),
           std::source_location where = std::source_location::current()) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (!done()) {
        INFO("waiting since line " << where.line());
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        step();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
std::unique_ptr<SessionTransport> open(const std::string& invitation = {}) {
    std::string error;
    auto transport = openSessionTransport({true, invitation}, error);
    REQUIRE(transport);
    until([&] { return transport->phase() != SessionTransport::Phase::Opening; }, [] {});
    INFO(transport->error());
    REQUIRE(transport->phase() == SessionTransport::Phase::Ready);
    return transport;
}
void configure(OnlineSession& session) {
    const OnlineSession::NativeIdentity identity{"adapter-test", "adapter-runtime",
                                                 std::string(64, 'a'), std::string(64, 'b'), "en"};
    OnlineSession::NativeRules rules;
    rules.scene = 1;
    rules.startupDelay = 0;
    REQUIRE(
        session.configureNative(identity, session.host() ? std::optional{rules} : std::nullopt));
}
void confirm(OnlineSession& session) {
    if (!session.nativeSetupPending()) {
        return;
    }
    test::confirmAgreedSetup(session, GameConfig{}, {});
}
template <class Step>
void exchangeFrames(std::span<OnlineSession* const> sessions,
                    std::span<test::RollbackSessionHarness* const> inputs, Step pump) {
    REQUIRE(sessions.size() == inputs.size());
    for (u64 tick = 1; tick <= 32; ++tick) {
        for (usize i = 0; i < sessions.size(); ++i) {
            auto* session = sessions[i];
            std::vector<InputCommand> commands;
            for (const auto seat : session->seats()) {
                InputCommand command;
                command.epoch = session->match().context().epoch;
                command.tick = tick;
                command.grant = session->match().context().grants[seat];
                command.seat = seat;
                command.direction = {1, 0};
                command.magnitude = 1;
                command.pressedButtons = tick % 3 == 0 ? static_cast<u32>(CommandPress::Attack) : 0;
                commands.push_back(command);
            }
            REQUIRE(inputs[i]->sample(commands));
            REQUIRE(session->flush());
        }
        // Local execution may predict the remote seats. Keep pumping the actual
        // provider and replaying its correction before inspecting confirmed output.
        until(
            [&] {
                return std::ranges::all_of(inputs, [tick](const auto* input) {
                    return input->tick() == tick + 1 && input->confirmedTick() >= tick;
                });
            },
            [&] {
                pump();
                for (usize i = 0; i < inputs.size(); ++i) {
                    inputs[i]->advance(); // no second forward tick without a local sample
                    REQUIRE(inputs[i]->complete());
                    REQUIRE(sessions[i]->flush());
                }
            });
        for (usize i = 0; i < sessions.size(); ++i) {
            const auto& frame = inputs[i]->lastFrame();
            REQUIRE(frame);
            for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
                if (sessions.front()->match().context().owners[seat] == 0) {
                    continue;
                }
                const auto& command = frame->commands[seat];
                CHECK(command.tick == tick);
                CHECK(command.seat == seat);
                CHECK(command.direction == Vec2{1, 0});
                CHECK(command.pressed(CommandPress::Attack) == (tick % 3 == 0));
                const auto bytes = InputPacket::encode(std::span(&command, 1));
                REQUIRE(bytes);
                CHECK(bytes == InputPacket::encode(
                                   std::span(&inputs.front()->lastFrame()->commands[seat], 1)));
            }
            CHECK(sessions[i]->match().nativeConfirmed() == tick);
        }
    }
}
} // namespace

TEST_CASE("contradictory transport path policies fail before opening a socket", "[netplay][iroh]") {
    std::string error;
    CHECK_FALSE(openSessionTransport({true, {}, true}, error));
    CHECK(error == "Local-only and relay-only transports are mutually exclusive");
}

TEST_CASE(
    "provider adapter carries native admission loading inputs and departure without global pause",
    "[netplay][iroh]") {
    auto wire = open();
    auto remote = open(wire->invitation());
    HostedRoom a(*wire, true, 2, "adapter-test", std::string(64, 'a'));
    HostedRoom b(*remote, false, 2, "adapter-test", std::string(64, 'a'));
    OnlineSession host(a, a.transport(), true, 2);
    OnlineSession guest(b, b.transport(), false, 2);
    configure(host);
    configure(guest);
    CharacterProfile profile;
    profile.name = "TEST";
    REQUIRE(host.select(std::vector<CharacterProfile>(2, profile)));
    REQUIRE(guest.select(std::vector<CharacterProfile>(2, profile)));
    const auto pump = [&] {
        host.update(0.002);
        guest.update(0.002);
        confirm(host);
        confirm(guest);
        REQUIRE(host.phase() != OnlineSession::Phase::Failed);
        REQUIRE(guest.phase() != OnlineSession::Phase::Failed);
    };
    until(
        [&] {
            return host.room() != nullptr && host.room()->members.size() == 2 && host.connected() &&
                   guest.connected();
        },
        pump);
    REQUIRE(guest.seats().size() == 2);
    CHECK(guest.seats()[0] == 2);
    CHECK(guest.seats()[1] == 3);
    REQUIRE(host.ready());
    REQUIRE(guest.ready());
    until([&] { return host.room()->members[1].ready; }, pump);
    REQUIRE(host.start());
    until(
        [&] {
            return host.phase() == OnlineSession::Phase::Active &&
                   guest.phase() == OnlineSession::Phase::Active;
        },
        pump);
    REQUIRE(host.match().prepare(1));
    until([&] { return guest.match().phase() == MatchSession::Phase::Loading; }, pump);
    test::RollbackSessionHarness hostInputs;
    REQUIRE(hostInputs.bind(host.match()));
    REQUIRE(host.match().loaded());
    for (s32 i = 0; i < 10; ++i) {
        pump();
    }
    CHECK(host.match().phase() == MatchSession::Phase::Loading);
    test::RollbackSessionHarness guestInputs;
    REQUIRE(guestInputs.bind(guest.match()));
    REQUIRE(guest.match().loaded());
    until(
        [&] {
            return guest.match().phase() == MatchSession::Phase::Running &&
                   host.match().phase() == MatchSession::Phase::Running;
        },
        pump);
    CHECK_FALSE(guest.match().requestPause());
    const std::array sessions{&host, &guest};
    const std::array inputs{&hostInputs, &guestInputs};
    exchangeFrames(sessions, inputs, pump);
    CHECK(host.match().phase() == MatchSession::Phase::Running);
    CHECK(guest.match().phase() == MatchSession::Phase::Running);
    // A guest that leaves mid-match is dropped; the host and its match carry on. Its
    // Stop may be lost with its connection, in which case the drop waits on the
    // transport noticing the closed link.
    guest.leave();
    until([&] { return host.match().dropped(2); },
          [&] {
              host.update(0.002);
              guest.update(0.002);
              INFO(host.failureDetail());
              REQUIRE(host.phase() != OnlineSession::Phase::Failed);
              REQUIRE(host.match().phase() != MatchSession::Phase::Stopped);
          },
          std::chrono::seconds(45));
    CHECK(host.phase() == OnlineSession::Phase::Active);
    CHECK(host.match().phase() == MatchSession::Phase::Running);
}

TEST_CASE("provider room rejects incompatible content without losing the host", "[netplay][iroh]") {
    auto wire = open();
    auto remote = open(wire->invitation());
    HostedRoom a(*wire, true, 1, "adapter-test", std::string(64, 'a'));
    HostedRoom b(*remote, false, 1, "adapter-test", std::string(64, 'b'));
    OnlineSession host(a, a.transport(), true, 1);
    OnlineSession guest(b, b.transport(), false, 1);
    until([&] { return guest.phase() == OnlineSession::Phase::Failed; },
          [&] {
              host.update(0.002);
              guest.update(0.002);
          });
    CHECK(b.failure() == HostedRoom::Failure::Assets);
    CHECK(host.phase() == OnlineSession::Phase::Lobby);
}

TEST_CASE("three independent provider peers admit a late third player and start together",
          "[netplay][iroh][three-peers]") {
    auto wire = open();
    auto remote = open(wire->invitation());
    HostedRoom a(*wire, true, 1, "adapter-test", std::string(64, 'a'));
    HostedRoom b(*remote, false, 1, "adapter-test", std::string(64, 'a'));
    OnlineSession host(a, a.transport(), true, 1);
    OnlineSession first(b, b.transport(), false, 1);
    configure(host);
    configure(first);
    CharacterProfile profile;
    profile.name = "TEST";
    REQUIRE(host.select(std::array{profile}));
    REQUIRE(first.select(std::array{profile}));
    const auto pumpPair = [&] {
        host.update(0.002);
        first.update(0.002);
        confirm(host);
        confirm(first);
        REQUIRE(host.phase() != OnlineSession::Phase::Failed);
        REQUIRE(first.phase() != OnlineSession::Phase::Failed);
    };
    until([&] { return host.connected() && first.connected(); }, pumpPair);
    REQUIRE(host.ready());
    REQUIRE(first.ready());
    until([&] { return host.room()->members[1].ready; }, pumpPair);
    auto thirdWire = open(wire->invitation());
    HostedRoom c(*thirdWire, false, 1, "adapter-test", std::string(64, 'a'));
    OnlineSession second(c, c.transport(), false, 1);
    configure(second);
    REQUIRE(second.select(std::array{profile}));
    usize steps = 0;
    const auto pump = [&] {
        pumpPair();
        // Unequal receive cadence, as when a distant guest batches incoming traffic.
        if (++steps % 5 == 0) {
            second.update(0.010);
            confirm(second);
        }
        INFO(second.failureDetail());
        REQUIRE(second.phase() != OnlineSession::Phase::Failed);
    };
    until(
        [&] {
            return host.room()->members.size() == 3 && first.room()->members.size() == 3 &&
                   second.connected() && host.connected();
        },
        pump);
    CHECK_FALSE(host.room()->members[0].ready);
    CHECK_FALSE(host.room()->members[1].ready);
    CHECK(first.seats()[0] == 1);
    CHECK(second.seats()[0] == 2);
    REQUIRE(host.ready());
    REQUIRE(first.ready());
    REQUIRE(second.ready());
    until([&] { return host.room()->members[1].ready && host.room()->members[2].ready; }, pump);
    REQUIRE(host.start());
    until(
        [&] {
            return host.phase() == OnlineSession::Phase::Active &&
                   first.phase() == OnlineSession::Phase::Active &&
                   second.phase() == OnlineSession::Phase::Active;
        },
        pump);
    REQUIRE(host.match().prepare(1));
    until(
        [&] {
            return first.match().phase() == MatchSession::Phase::Loading &&
                   second.match().phase() == MatchSession::Phase::Loading;
        },
        pump);
    test::RollbackSessionHarness hostInputs;
    REQUIRE(hostInputs.bind(host.match()));
    REQUIRE(host.match().loaded());
    test::RollbackSessionHarness firstInputs;
    REQUIRE(firstInputs.bind(first.match()));
    REQUIRE(first.match().loaded());
    for (usize i = 0; i < 50; ++i) {
        pump();
    }
    CHECK(host.match().phase() == MatchSession::Phase::Loading);
    test::RollbackSessionHarness secondInputs;
    REQUIRE(secondInputs.bind(second.match()));
    REQUIRE(second.match().loaded());
    until(
        [&] {
            return host.match().phase() == MatchSession::Phase::Running &&
                   first.match().phase() == MatchSession::Phase::Running &&
                   second.match().phase() == MatchSession::Phase::Running;
        },
        pump);
    const std::array sessions{&host, &first, &second};
    const std::array inputs{&hostInputs, &firstInputs, &secondInputs};
    exchangeFrames(sessions, inputs, pump);
}
