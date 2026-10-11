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

#include "engine/io/File.h"
#include "engine/net/SessionTransport.h"

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

void host(SessionTransport& transport, usize clients) {
    struct Client {
        u8 seat = 0;
        u8 ordered = 0;
        u64 accepted = 0;
        u64 duplicates = 0;
        bool finished = false;
        bool acknowledged = false;
        bool closed = false;
    };
    report({{"event", "listening"}});
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

void client(SessionTransport& transport) {
    std::optional<Connection> connection;
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
            if (event.type == EventType::Connected) {
                require(!connection, "Unexpected second host");
                connection = event.connection;
                continue;
            }
            require(connection && event.connection == *connection,
                    "Unexpected connection identity");
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
                            {"relayed", stats->relayed},
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

void ready(SessionTransport& transport) {
    const auto deadline = Clock::now() + std::chrono::seconds(25);
    while (transport.phase() == SessionTransport::Phase::Opening && Clock::now() < deadline) {
        waitForNextPoll();
    }
    require(transport.phase() == SessionTransport::Phase::Ready,
            "Network setup failed: " + transport.error());
}
void selfTest() {
    std::string error;
    require(!openSessionTransport({true, "bad invitation"}, error), "Malformed invite accepted");
    for (s32 cycle = 0; cycle < 2; ++cycle) {
        auto host = openSessionTransport({true, {}}, error);
        require(host != nullptr, error);
        ready(*host);
        auto guest = openSessionTransport({true, host->invitation()}, error);
        require(guest != nullptr, error);
        ready(*guest);
        Connection id = 0;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (id == 0 && Clock::now() < deadline) {
            for (const auto& event : guest->poll()) {
                if (event.type == EventType::Connected) {
                    id = event.connection;
                }
            }
            waitForNextPoll();
        }
        require(id != 0, "Guest never became connected");
        require(guest->send(id, {}, Delivery::Reliable) == PacketTransport::SendResult::Invalid,
                "Empty packet accepted");
        const std::vector<u8> large(PacketTransport::kMaxPacketBytes + 2);
        require(guest->send(id, large, Delivery::Reliable) == PacketTransport::SendResult::Invalid,
                "Oversized packet accepted");
        const std::array<u8, PacketTransport::kMaxPacketBytes> bytes{};
        bool congested = false;
        for (usize attempt = 0; attempt < 4096 && !congested; ++attempt) {
            const auto result = guest->send(id, bytes, Delivery::Reliable);
            require(result == PacketTransport::SendResult::Sent ||
                        result == PacketTransport::SendResult::Congested,
                    "Invalid pressure result");
            congested = result == PacketTransport::SendResult::Congested;
        }
        require(congested, "Reliable send queue did not apply backpressure");
        require(host->poll().size() <= PacketTransport::kReceiveBudget, "Receive budget exceeded");
        guest->close(id);
        require(!guest->statistics(id), "Closed connection remains live");
        require(guest->send(id, bytes, Delivery::Reliable) ==
                    PacketTransport::SendResult::Disconnected,
                "Closed connection accepts messages");
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
        require(argc == 4 || argc == 5,
                "Usage: netplaycheck self-test | host INVITE_FILE CLIENTS [internet] | "
                "client INVITE_FILE unused [internet]");
        const std::string role = arguments[1];
        require(role == "host" || role == "client", "Unknown role");
        const bool local = argc != 5;
        if (!local) {
            require(std::string(arguments[4]) == "internet", "Unknown scope");
        }
        const std::filesystem::path ticketPath = arguments[2];
        const auto invite = role == "host" ? std::string{} : readTextFile(ticketPath);
        std::string error;
        auto transport = openSessionTransport({local, invite}, error);
        require(transport != nullptr, error);
        ready(*transport);
        if (role == "host") {
            const auto clients = std::stoi(arguments[3]);
            require(clients >= 1 && clients <= 3, "Invalid client count");
            require(!std::filesystem::exists(ticketPath), "Invitation output already exists");
            replaceTextFile(ticketPath, transport->invitation());
            host(*transport, static_cast<usize>(clients));
        } else {
            client(*transport);
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
