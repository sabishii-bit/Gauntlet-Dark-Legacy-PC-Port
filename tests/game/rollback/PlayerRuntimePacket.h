#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "game/party/PlayerRuntimeCheckpoint.h"

namespace gdl::game {
/** Complete in-memory participant state, not a profile or a save-file operation.
 * Resource names and poses are descriptive bindings: only an already-loaded
 * PlayerRuntimeCheckpoint owner can validate and restore them. No packet value
 * creates an asset, chooses a filesystem path, or replaces a persistent save. */
class PlayerRuntimePacket {
public:
    static constexpr u32 kVersion = 4;
    static constexpr usize kHeaderBytes = 16;
    static constexpr usize kMaxBytes = static_cast<const usize>(512 * 1024);
    static constexpr usize kMaxRecords = 4096;
    static constexpr usize kMaxMovies = 256;
    static constexpr usize kMaxString = 4096;

    /** Locally valid states outside these transport bounds are refused intact. */
    static std::optional<std::vector<u8>> encode(const PlayerRuntimeCheckpoint::State& state);
    static std::optional<PlayerRuntimeCheckpoint::State> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
