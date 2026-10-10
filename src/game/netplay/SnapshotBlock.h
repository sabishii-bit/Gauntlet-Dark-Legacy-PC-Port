#pragma once

#include "game/netplay/CombatSnapshot.h"

namespace gdl::game {
/** Lossless, independently decodable checkpoint envelope. No dictionary/baseline
 * survives between packets, and incompressible blocks are sent verbatim. Dense
 * scalar data optionally uses reversible byte planes to improve LZ4 compression.
 * No floating-point precision is lost. Limits
 * are checked before allocating output or invoking the bounded LZ4 decoder.
 * Integrity/authentication remain the containing channel's responsibility. */
class SnapshotBlock {
public:
    enum class Compression : u8 { None, Automatic };
    static constexpr usize kHeaderBytes = 12;
    static constexpr usize kMaxBytes = kHeaderBytes + CombatPacket::kMaxBytes;
    static std::optional<std::vector<u8>> encode(std::span<const u8> bytes,
                                                 Compression compression = Compression::Automatic);
    static std::optional<std::vector<u8>> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
