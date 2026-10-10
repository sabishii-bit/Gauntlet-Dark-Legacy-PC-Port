#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/math/Math.h"

namespace gdl::game {
/** One authoritative fighter draw, not an AI state. Actor 0 is the encounter;
 * 1..16 are population slots. Incarnations disambiguate reuse. Parts are stable
 * within an incarnation; resource IDs refer only to a preloaded model roster. */
struct FighterMeshState {
    static constexpr u32 kUnlit = 1;
    static constexpr u32 kFlash = 2;
    static constexpr u32 kFrozen = 4;
    struct Node {
        Mat4 transform{1};
        u64 generation = 0;
        u32 sequence = 0;
        f32 frame = 0;
        f32 alpha = 1;
        bool flash = false;
    };
    u32 actor = 0;
    u64 incarnation = 0;
    u32 part = 0;
    u32 resource = 0;
    Mat4 placement{1};
    u32 sequence = 0;
    f32 frame = 0;
    f32 textureClock = 0;
    f32 alpha = 1;
    Color tint = Color::white();
    u32 skin = 0;
    u32 flags = 0;
    std::vector<Node> nodes;
    auto key() const { return std::pair{actor, part}; }
    bool valid() const;
};

class FighterPacket {
public:
    static constexpr usize kMaxMeshes = 256;
    static constexpr usize kMaxNodes = 4096;
    static constexpr usize kMaxTreeNodes = 256;
    static constexpr usize kHeaderBytes = 8;
    static constexpr usize kMeshBytes = 100;
    static constexpr usize kNodeBytes = 72;
    static constexpr usize kMaxBytes =
        kHeaderBytes + kMaxMeshes * kMeshBytes + kMaxNodes * kNodeBytes;
    static bool valid(std::span<const FighterMeshState> meshes);
    static std::optional<std::vector<u8>> encode(std::span<const FighterMeshState> meshes);
    static std::optional<std::vector<FighterMeshState>> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
