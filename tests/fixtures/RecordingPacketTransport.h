#pragma once

#include <limits>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/core/Types.h"
#include "engine/net/PacketTransport.h"

namespace gdl::test {
/** A transport double that keeps every send it accepts for the test to hand on, and refuses
 * or congests when a case asks it to. */
struct RecordingPacketTransport : PacketTransport {
    struct Packet {
        Connection connection = 0;
        std::vector<u8> bytes;
        Delivery delivery = Delivery::Unreliable;
    };
    std::vector<Packet> sent;
    SendResult result = SendResult::Sent;             ///< returned instead of sending
    Connection faultConnection = 0;                   ///< where `result` applies; zero for all
    usize budget = std::numeric_limits<usize>::max(); ///< accepted sends before Congested
    std::map<Connection, usize> budgets;              ///< a connection's own budget, used instead
    usize attempts = 0;                               ///< every send asked for, accepted or not

    /** Counts the attempt and shows it to inspect; then a matching fault answers `result`
     * without spending budget, and a spent budget answers Congested. */
    SendResult send(Connection connection, std::span<const u8> bytes, Delivery delivery) override {
        ++attempts;
        inspect(connection, bytes, delivery);
        if (result != SendResult::Sent && (faultConnection == 0 || faultConnection == connection)) {
            return result;
        }
        const auto own = budgets.find(connection);
        usize& remaining = own != budgets.end() ? own->second : budget;
        if (remaining == 0) {
            return SendResult::Congested;
        }
        --remaining;
        sent.push_back({connection, {bytes.begin(), bytes.end()}, delivery});
        return SendResult::Sent;
    }
    std::vector<Event> poll() override { return {}; }
    std::optional<Statistics> statistics(Connection /*connection*/) const override {
        return std::nullopt;
    }
    void close(Connection /*connection*/) override {}
    /** Hands over what was sent since the last take. */
    std::vector<Packet> take() { return std::exchange(sent, {}); }

protected:
    /** A suite's checks on every send, refused ones included (delivery mode, size). */
    virtual void inspect(Connection /*connection*/, std::span<const u8> /*bytes*/,
                         Delivery /*delivery*/) {}
};
} // namespace gdl::test
