#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/net/GnsTransport.h"

#include "game/netplay/InputCommand.h"
#include "game/netplay/InputTimeline.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Clock = std::chrono::steady_clock;
using Connection = PacketTransport::Connection;
using Delivery = PacketTransport::Delivery;
using Event = PacketTransport::Event;
using EventType = PacketTransport::EventType;

// Diagnostic control protocol only, never a game room protocol. Input payloads
// use the actual game codec/timeline. Separate processes exercise real UDP,
// library shutdown, reliable ordering and loss without a renderer or disc assets.
enum class Control : u8 { Welcome = 240, Ordered, Finish, Done, Acknowledge, Goodbye };
constexpr u64 kTicks = 120;
constexpr u8 kControls = 15;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void report(const nlohmann::json& message) {
    std::cout << message.dump() << '\n';
    std::cout.flush(); // The runner must see the listening port before launching clients.
}

void sendControl(PacketTransport& transport, Connection connection, Control type, u8 value = 0) {
    const std::array<u8, 2> bytes{static_cast<u8>(type), value};
    require(transport.send(connection, bytes, Delivery::Reliable) ==
                PacketTransport::SendResult::Sent,
            "Reliable control could not be queued");
}

bool control(const Event& event, Control type) {
    return event.bytes.size() == 2 && event.bytes[0] == static_cast<u8>(type);
}

void waitForNextPoll() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void host(GnsTransport& transport, usize clients) {
    struct Client {
        u8 seat = 0;
        u8 ordered = 0;
        u64 accepted = 0;
        u64 duplicates = 0;
        bool finished = false;
        bool acknowledged = false;
        bool closed = false;
    };
    const auto port = transport.listenLoopback();
    require(port.has_value(), "Could not bind loopback listener");
    report({{"event", "listening"}, {"port", *port}});
    InputTimeline timeline;
    std::map<Connection, Client> peers;
    const auto deadline = Clock::now() + std::chrono::seconds(25);
    while (Clock::now() < deadline) {
        for (const auto& event : transport.poll()) {
            if (event.type == EventType::Connected) {
                require(peers.size() < clients, "Unexpected extra connection");
                const auto seat = static_cast<u8>(peers.size());
                peers.emplace(event.connection, Client{seat});
                require(timeline.assign(seat, event.connection), "Seat assignment failed");
                sendControl(transport, event.connection, Control::Welcome, seat);
                continue;
            }
            auto found = peers.find(event.connection);
            require(found != peers.end(), "Event for an unknown connection");
            auto& peer = found->second;
            if (event.type == EventType::Disconnected) {
                require(peer.acknowledged, "Peer disconnected before finishing: " + event.reason);
                peer.closed = true;
                timeline.disconnect(event.connection);
                require(timeline.owner(peer.seat) == 0, "Disconnected peer retained a seat");
                continue;
            }
            if (control(event, Control::Ordered)) {
                require(event.bytes[1] == peer.ordered && peer.ordered < kControls,
                        "Reliable controls arrived out of order or duplicated");
                ++peer.ordered;
                sendControl(transport, event.connection, Control::Ordered, event.bytes[1]);
            } else if (control(event, Control::Finish)) {
                require(peer.ordered == kControls && !peer.finished,
                        "Finish overtook a reliable control");
                require(peer.accepted >= kTicks / 2, "Too few game inputs reached the host");
                peer.finished = true;
                sendControl(transport, event.connection, Control::Done);
                report({{"event", "peer_done"},
                        {"seat", peer.seat},
                        {"accepted", peer.accepted},
                        {"duplicates", peer.duplicates}});
            } else if (control(event, Control::Acknowledge)) {
                require(peer.finished && !peer.acknowledged,
                        "Unexpected completion acknowledgement");
                peer.acknowledged = true;
                sendControl(transport, event.connection, Control::Goodbye);
            } else {
                const auto commands = InputPacket::decode(event.bytes);
                require(commands.has_value(), "Invalid input packet on the wire");
                for (const auto& command : *commands) {
                    require(command.seat == peer.seat && command.tick < kTicks &&
                                command.held(CommandHeld::Attack),
                            "Input payload changed or crossed connection ownership");
                    const auto admission = timeline.submit(event.connection, command);
                    require(admission == InputTimeline::Admission::Accepted ||
                                admission == InputTimeline::Admission::Duplicate,
                            "Valid remote input rejected by host admission");
                    if (admission == InputTimeline::Admission::Accepted) {
                        ++peer.accepted;
                    } else {
                        ++peer.duplicates;
                    }
                }
            }
        }
        if (peers.size() == clients &&
            std::ranges::all_of(peers, [](const auto& pair) { return pair.second.closed; })) {
            report({{"event", "passed"}, {"role", "host"}, {"clients", clients}});
            return;
        }
        waitForNextPoll();
    }
    throw std::runtime_error("Host timed out waiting for clients");
}

void client(GnsTransport& transport, u16 port) {
    const auto connection = transport.connectLoopback(port);
    require(connection.has_value(), "Could not connect to loopback host");
    std::optional<u8> seat;
    u64 tick = 0;
    u8 echoed = 0;
    bool finished = false;
    bool acknowledged = false;
    u64 congested = 0;
    InputHistory history;
    auto nextTick = Clock::now();
    const auto deadline = nextTick + std::chrono::seconds(20);
    while (Clock::now() < deadline) {
        for (const auto& event : transport.poll()) {
            require(event.connection == *connection, "Unexpected connection identity");
            require(event.type != EventType::Disconnected, "Host disconnected: " + event.reason);
            if (event.type != EventType::Message) {
                continue;
            }
            if (control(event, Control::Welcome)) {
                require(!seat && event.bytes[1] < InputCommand::kSeats, "Invalid welcome");
                seat = event.bytes[1];
                nextTick = Clock::now();
            } else if (control(event, Control::Ordered)) {
                require(event.bytes[1] == echoed && echoed < kControls, "Unordered control echo");
                ++echoed;
            } else if (control(event, Control::Done)) {
                require(finished && echoed == kControls && !acknowledged, "Premature completion");
                acknowledged = true;
                if (const auto stats = transport.statistics(*connection)) {
                    report({{"event", "statistics"},
                            {"ping_ms", stats->pingMs},
                            {"queued_bytes", stats->pendingBytes},
                            {"queue_us", stats->queueMicroseconds}});
                }
                sendControl(transport, *connection, Control::Acknowledge);
            } else if (control(event, Control::Goodbye)) {
                require(acknowledged, "Premature goodbye");
                transport.close(*connection);
                require(!transport.statistics(*connection), "Closed connection remains live");
                const std::array<u8, 1> probe{1};
                require(transport.send(*connection, probe, Delivery::Unreliable) ==
                            PacketTransport::SendResult::Disconnected,
                        "Closed connection still accepts messages");
                report({{"event", "passed"},
                        {"role", "client"},
                        {"ticks", tick},
                        {"congested", congested},
                        {"ordered_controls", echoed}});
                return;
            } else {
                throw std::runtime_error("Unknown host control");
            }
        }
        if (seat && tick < kTicks && Clock::now() >= nextTick) {
            InputCommand command;
            command.epoch = 1;
            command.grant = 1;
            command.seat = *seat;
            command.tick = tick;
            command.direction = {1, 0};
            command.magnitude = 1;
            command.heldButtons = static_cast<u32>(CommandHeld::Attack);
            require(history.record(std::span(&command, 1)), "Could not record input history");
            const auto packet = InputPacket::encode(history.commands());
            require(packet.has_value(), "Could not encode input history");
            const auto result = transport.send(*connection, *packet, Delivery::Unreliable);
            require(result == PacketTransport::SendResult::Sent ||
                        result == PacketTransport::SendResult::Congested,
                    "Input send failed");
            congested += result == PacketTransport::SendResult::Congested ? 1U : 0U;
            if (tick % 8 == 0) {
                sendControl(transport, *connection, Control::Ordered, static_cast<u8>(tick / 8));
            }
            ++tick;
            nextTick += std::chrono::microseconds(16667);
        }
        if (tick == kTicks && !finished) {
            sendControl(transport, *connection, Control::Finish);
            finished = true;
        }
        waitForNextPoll();
    }
    throw std::runtime_error("Client timed out waiting for host");
}

void selfTest() {
    std::string error;
    require(!GnsTransport::create({-1, 0}, error) && !error.empty(), "Invalid profile accepted");
    for (s32 cycle = 0; cycle < 2; ++cycle) {
        auto transport = GnsTransport::create({}, error);
        require(transport != nullptr, error);
        require(!GnsTransport::create({}, error), "Second transport stole process runtime");
        const auto port = transport->listenLoopback();
        require(port.has_value(), "Listener failed after initialization");
        require(!transport->listenLoopback(), "Second listener leaked the first");
        require(!transport->connectLoopback(0), "Invalid port accepted");
        require(transport->send(1, {}, Delivery::Reliable) == PacketTransport::SendResult::Invalid,
                "Empty packet accepted");
        const std::vector<u8> large(PacketTransport::kMaxPacketBytes + 1);
        require(transport->send(1, large, Delivery::Reliable) ==
                    PacketTransport::SendResult::Invalid,
                "Oversized packet accepted");
        require(!transport->statistics(1), "Unknown connection returned statistics");
        transport->close(1);

        const auto source = transport->connectLoopback(*port);
        require(source.has_value(), "Self-connect failed");
        usize connected = 0;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (connected < 2 && Clock::now() < deadline) {
            for (const auto& event : transport->poll()) {
                require(event.type == EventType::Connected, "Self-connect failed during handshake");
                ++connected;
            }
            waitForNextPoll();
        }
        require(connected == 2, "Self-connect timed out");
        const std::array<u8, PacketTransport::kMaxPacketBytes> bytes{};
        bool congested = false;
        for (usize attempt = 0; attempt < 4096 && !congested; ++attempt) {
            const auto result = transport->send(*source, bytes, Delivery::Reliable);
            require(result == PacketTransport::SendResult::Sent ||
                        result == PacketTransport::SendResult::Congested,
                    "Unexpected result while filling send queue");
            congested = result == PacketTransport::SendResult::Congested;
        }
        require(congested, "Send queue did not apply backpressure");
        require(transport->poll().size() <=
                    PacketTransport::kReceiveBudget + PacketTransport::kMaxConnections * 2,
                "Receive poll exceeded its work budget");
        transport->close(*source);
        require(!transport->statistics(*source), "Closed connection survived teardown");
        require(transport->send(*source, bytes, Delivery::Reliable) ==
                    PacketTransport::SendResult::Disconnected,
                "Stale connection accepted a packet");
        const auto replacement = transport->connectLoopback(*port);
        require(replacement.has_value() && *replacement != *source,
                "Connection identity was reused");
    }
    report({{"event", "passed"}, {"role", "self-test"}});
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::span arguments(argv, static_cast<usize>(argc));
        if (argc == 2 && std::string(arguments[1]) == "self-test") {
            selfTest();
            return 0;
        }
        require(argc == 5, "Usage: netplaycheck self-test | host CLIENTS LAG_MS LOSS_PERCENT | "
                           "client PORT LAG_MS LOSS_PERCENT");
        const std::string role = arguments[1];
        const s32 value = std::stoi(arguments[2]);
        require(role == "host" || role == "client", "Unknown role");
        require(role == "host" ? value >= 1 && value <= 3 : value > 0 && value <= 65535,
                "Invalid client count or port");
        std::string error;
        auto transport =
            GnsTransport::create({std::stoi(arguments[3]), std::stof(arguments[4])}, error);
        require(transport != nullptr, error);
        if (role == "host") {
            host(*transport, static_cast<usize>(value));
        } else {
            client(*transport, static_cast<u16>(value));
        }
        return 0;
    } catch (const std::exception& error) {
        // Failure reporting must work even if JSON serialization/allocation failed.
        std::fputs("Network test failed: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
