#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "game/enemies/Enemies.h"

namespace gdl::game {

/** Complete native swarm state, explicitly little-endian. Decoding establishes
 * bounded value syntax only; Enemies::prepareRestore resolves the named roster
 * against already-loaded trusted resources before any state becomes live.
 * Transport limits intentionally reject some larger locally valid checkpoints;
 * no queue, body, pose or contact is silently omitted to make a packet fit. */
class EnemyPoolCheckpoint {
public:
    /** Canonical encoded-byte SHA, not validation of incoming state. */
    static std::optional<std::array<u8, 32>> canonicalDigest(const Enemies::Checkpoint& state);

    static constexpr u32 kVersion = 5;
    static constexpr usize kMaxBytes = static_cast<const usize>(512 * 1024);
    static constexpr usize kMaxNodes = 128;
    static constexpr usize kMaxTotalNodes = 2048;
    static constexpr usize kMaxEvents = 256;
    static constexpr usize kMaxTotalEvents = 1024;
    static constexpr usize kMaxCombatants = 256;

    static std::optional<std::vector<u8>> encode(const Enemies::Checkpoint& state);
    static std::optional<Enemies::Checkpoint> decode(std::span<const u8> bytes);
};

} // namespace gdl::game
