#pragma once

#include <algorithm>
#include <span>
#include <vector>

#include "game/netplay/InputTimeline.h"
#include "game/netplay/SnapshotPlayback.h"

namespace gdl::test {

/** Deterministic transport impairment, not a socket mock that delivers structs directly.
 * Time is host ticks. Jitter can reorder packets; loss and duplication use independent
 * repeatable schedules. Seedless schedules make every failure reproducible in CI. */
class ImpairedInputLink {
public:
    struct Profile {
        u64 latency = 0;
        u64 jitter = 0;
        u64 dropEvery = 0;
        u64 duplicateEvery = 0;
        u64 duplicateDelay = 0;
    };
    explicit ImpairedInputLink(Profile profile) : m_profile(profile) {}

    void send(u64 now, game::InputTimeline::Peer peer, std::span<const u8> bytes) {
        const u64 serial = ++m_sent;
        if (m_profile.dropEvery != 0 && serial % m_profile.dropEvery == 0) {
            ++m_dropped;
            return;
        }
        const u64 due =
            now + m_profile.latency + (m_profile.jitter == 0 ? 0 : serial % (m_profile.jitter + 1));
        const Delivery delivery{due, serial, peer, std::vector<u8>(bytes.begin(), bytes.end())};
        m_pending.push_back(delivery);
        if (m_profile.duplicateEvery != 0 && serial % m_profile.duplicateEvery == 0) {
            auto duplicate = delivery;
            duplicate.due += m_profile.duplicateDelay;
            m_pending.push_back(duplicate);
            ++m_duplicated;
        }
    }

    std::vector<game::InputTimeline::Admission> deliver(u64 now, game::InputTimeline& host) {
        std::vector<game::InputTimeline::Admission> admissions;
        drain(now, [&](const Delivery& delivery) {
            const auto commands = game::InputPacket::decode(delivery.bytes);
            if (!commands) {
                ++m_malformed;
                return;
            }
            for (const auto& command : *commands) {
                admissions.push_back(host.submit(delivery.peer, command));
            }
        });
        return admissions;
    }

    std::vector<game::SnapshotPlayback::Admission> deliver(u64 now,
                                                           game::SnapshotPlayback& client) {
        std::vector<game::SnapshotPlayback::Admission> admissions;
        drain(now, [&](const Delivery& delivery) {
            admissions.push_back(client.receive(delivery.peer, delivery.bytes));
        });
        return admissions;
    }

    u64 dropped() const { return m_dropped; }
    u64 duplicated() const { return m_duplicated; }
    u64 reordered() const { return m_reordered; }
    u64 malformed() const { return m_malformed; }

private:
    struct Delivery {
        u64 due = 0;
        u64 serial = 0;
        game::InputTimeline::Peer peer = 0;
        std::vector<u8> bytes;
    };
    template <class Receive> void drain(u64 now, Receive receive) {
        std::ranges::stable_sort(m_pending, {}, &Delivery::due);
        for (const auto& delivery : m_pending) {
            if (delivery.due > now) {
                break;
            }
            if (delivery.serial < m_lastDelivered) {
                ++m_reordered;
            }
            m_lastDelivered = std::max(delivery.serial, m_lastDelivered);
            receive(delivery);
        }
        std::erase_if(m_pending, [now](const auto& delivery) { return delivery.due <= now; });
    }
    Profile m_profile;
    std::vector<Delivery> m_pending;
    u64 m_sent = 0;
    u64 m_dropped = 0;
    u64 m_duplicated = 0;
    u64 m_lastDelivered = 0;
    u64 m_reordered = 0;
    u64 m_malformed = 0;
};

} // namespace gdl::test
