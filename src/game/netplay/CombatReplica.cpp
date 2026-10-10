#include "game/netplay/CombatReplica.h"

#include <algorithm>

#include "engine/io/ByteReader.h"

namespace gdl::game {
namespace {
constexpr u32 kMagic = fourcc("GDLC");
constexpr u16 kVersion = 4;
// Frame consistency only, not authentication (provided by the host connection).
// Prevents merging fragments from two accidentally different captures of one tick.
u32 checksum(std::span<const u8> bytes) {
    u32 result = 2166136261U;
    for (const u8 byte : bytes) {
        result = (result ^ byte) * 16777619U;
    }
    return result;
}
void word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}
void wide(std::vector<u8>& bytes, u64 value) {
    word(bytes, static_cast<u32>(value));
    word(bytes, static_cast<u32>(value >> 32U));
}
u64 wide(ByteReader& reader) {
    const u64 low = reader.readU32();
    return low | (static_cast<u64>(reader.readU32()) << 32U);
}
static_assert(CombatReplica::kHeaderBytes + CombatReplica::kChunkBytes <=
              PacketTransport::kMaxPacketBytes);
static_assert(CombatReplica::kMaxPackets <= 65535);
} // namespace

std::optional<CombatReplica::Packets> CombatReplica::packets(const CombatSnapshot& snapshot,
                                                             SnapshotBlock::Compression compression,
                                                             Recovery recovery) {
    const auto raw = CombatPacket::encode(snapshot);
    if (!raw) {
        return std::nullopt;
    }
    const auto payload = SnapshotBlock::encode(*raw, compression);
    if (!payload) {
        return std::nullopt;
    }
    const usize count = (payload->size() + kChunkBytes - 1) / kChunkBytes;
    const usize repairs = recovery == Recovery::SingleLoss && count > 1
                              ? (count + kRepairGroup - 1) / kRepairGroup
                              : 0;
    const u32 digest = checksum(*payload);
    Packets packets;
    packets.reserve(count + repairs);
    for (usize index = 0; index < count + repairs; ++index) {
        std::vector<u8> bytes;
        std::vector<u8> parity;
        std::span<const u8> chunk;
        if (index < count) {
            const usize offset = index * kChunkBytes;
            chunk = std::span(*payload).subspan(offset,
                                                std::min(kChunkBytes, payload->size() - offset));
        } else {
            parity.resize(kChunkBytes);
            const usize first = (index - count) * kRepairGroup;
            for (usize part = first; part < std::min(first + kRepairGroup, count); ++part) {
                const usize offset = part * kChunkBytes;
                for (usize byte = 0; byte < std::min(kChunkBytes, payload->size() - offset);
                     ++byte) {
                    parity[byte] ^= (*payload)[offset + byte];
                }
            }
            chunk = parity;
        }
        bytes.reserve(kHeaderBytes + chunk.size());
        word(bytes, kMagic);
        word(bytes, kVersion);
        wide(bytes, snapshot.motion.epoch);
        wide(bytes, snapshot.motion.tick);
        word(bytes, static_cast<u32>(payload->size()));
        word(bytes, digest);
        word(bytes, static_cast<u32>(count) | (static_cast<u32>(index) << 16U));
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
        packets.push_back(std::move(bytes));
    }
    return packets;
}

bool CombatReplica::begin(PacketTransport::Connection host, u64 epoch) {
    if (host == 0 || epoch == 0 || (m_host != 0 && (host != m_host || epoch <= m_epoch))) {
        return false;
    }
    clear();
    m_host = host;
    m_epoch = epoch;
    return true;
}
void CombatReplica::clear() {
    m_host = 0;
    m_epoch = 0;
    m_pending.clear();
    m_latest.reset();
}

CombatReplica::Admission CombatReplica::receive(PacketTransport::Connection sender,
                                                std::span<const u8> bytes) {
    if (m_host == 0 || sender != m_host) {
        return Admission::WrongHost;
    }
    if (bytes.size() <= kHeaderBytes || bytes.size() > kHeaderBytes + kChunkBytes) {
        return Admission::Invalid;
    }
    ByteReader reader(bytes);
    if (reader.readU32() != kMagic || reader.readU16() != kVersion) {
        return Admission::Invalid;
    }
    if (reader.readU16() != 0) {
        return Admission::Invalid;
    }
    const u64 epoch = wide(reader);
    const u64 tick = wide(reader);
    const usize total = reader.readU32();
    const u32 digest = reader.readU32();
    const usize count = reader.readU16();
    const usize index = reader.readU16();
    const usize repairs = count > 1 ? (count + kRepairGroup - 1) / kRepairGroup : 0;
    if (total <= SnapshotBlock::kHeaderBytes || total > SnapshotBlock::kMaxBytes ||
        count != (total + kChunkBytes - 1) / kChunkBytes || index >= count + repairs ||
        bytes.size() - kHeaderBytes !=
            (index >= count ? kChunkBytes : std::min(kChunkBytes, total - index * kChunkBytes))) {
        return Admission::Invalid;
    }
    if (epoch != m_epoch) {
        return Admission::WrongEpoch;
    }
    if (m_latest && tick <= m_latest->motion.tick) {
        return Admission::Stale;
    }
    auto found = m_pending.find(tick);
    if (found == m_pending.end()) {
        if (m_pending.size() == kPendingSnapshots) {
            if (tick < m_pending.begin()->first) {
                return Admission::Stale;
            }
            m_pending.erase(m_pending.begin());
        }
        found = m_pending
                    .emplace(tick, Assembly{total, digest,
                                            std::vector<std::optional<std::vector<u8>>>(count),
                                            std::vector<std::optional<std::vector<u8>>>(repairs)})
                    .first;
    }
    auto& assembly = found->second;
    if (assembly.bytes != total || assembly.checksum != digest || assembly.chunks.size() != count) {
        return Admission::Invalid;
    }
    const auto payload = bytes.subspan(kHeaderBytes);
    auto& chunk = index < count ? assembly.chunks[index] : assembly.repair[index - count];
    if (chunk) {
        return std::ranges::equal(*chunk, payload) ? Admission::Duplicate : Admission::Invalid;
    }
    chunk = std::vector<u8>(payload.begin(), payload.end());
    for (usize group = 0; group < repairs; ++group) {
        if (!assembly.repair[group]) {
            continue;
        }
        const usize first = group * kRepairGroup;
        const usize end = std::min(first + kRepairGroup, count);
        usize missing = count;
        usize holes = 0;
        for (usize part = first; part < end; ++part) {
            if (!assembly.chunks[part]) {
                missing = part;
                ++holes;
            }
        }
        if (holes != 1) {
            continue;
        }
        auto restored = *assembly.repair[group];
        for (usize part = first; part < end; ++part) {
            if (assembly.chunks[part]) {
                const auto& present = *assembly.chunks[part];
                for (usize byte = 0; byte < present.size(); ++byte) {
                    restored[byte] ^= present[byte];
                }
            }
        }
        restored.resize(std::min(kChunkBytes, total - missing * kChunkBytes));
        assembly.chunks[missing] = std::move(restored);
    }
    if (std::ranges::any_of(assembly.chunks, [](const auto& part) { return !part; })) {
        return Admission::Pending;
    }
    std::vector<u8> joined;
    joined.reserve(total);
    for (const auto& part : assembly.chunks) {
        if (part) {
            joined.insert(joined.end(), part->begin(), part->end());
        }
    }
    m_pending.erase(found);
    if (checksum(joined) != digest) {
        return Admission::Invalid;
    }
    const auto raw = SnapshotBlock::decode(joined);
    const auto decoded = raw ? CombatPacket::decode(*raw) : std::nullopt;
    if (!decoded || decoded->motion.epoch != epoch || decoded->motion.tick != tick) {
        return Admission::Invalid;
    }
    m_latest = *decoded;
    std::erase_if(m_pending, [tick](const auto& row) { return row.first <= tick; });
    return Admission::Committed;
}

} // namespace gdl::game
