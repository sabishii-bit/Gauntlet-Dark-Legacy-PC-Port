#include "expansions/n64/enemies/LegendsEnemySounds.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::game {
LegendsEnemySounds LegendsEnemySounds::decode(std::span<const u8> program,
                                              std::span<const u8> enemies) {
    LegendsEnemySounds result;
    result.m_families.fill(-1);
    constexpr usize kTable = 0xe005fbb0U - 0xe0000400U;
    constexpr usize kBytes = usize{7} * 15 * 2;
    if (program.size() < kTable + kBytes || enemies.size() < 12) {
        throw FormatError("N64 enemy sound tables are truncated");
    }
    for (usize family = 0; family < result.m_cues.size(); ++family) {
        for (usize cue = 0; cue < result.m_cues[family].size(); ++cue) {
            result.m_cues[family][cue] = readU16BE(program, kTable + (family * 15 + cue) * 2);
        }
    }
    // e005f7a4 matches each FOURCC, copying the following 48 bytes to the
    // species table. e005c270 indexes its voice family at byte 39. Each row
    // contains six melee slots, six hurt slots, and three death slots.
    constexpr std::array<std::pair<u32, usize>, 17> kKinds{{{0x53434f52, 0},
                                                            {0x54524f4c, 1},
                                                            {0x44454d4f, 2},
                                                            {0x00524154, 3},
                                                            {0x4752554e, 4},
                                                            {0x4b4e4947, 5},
                                                            {0x004d4147, 12},
                                                            {0x005a4f4d, 13},
                                                            {0x504c4147, 14},
                                                            {0x574f4c46, 15},
                                                            {0x00494345, 16},
                                                            {0x574f524d, 17},
                                                            {0x534b4544, 18},
                                                            {0x534b454c, 19},
                                                            {0x474f4c4d, 29},
                                                            {0x44455448, 30},
                                                            {0x00004954, 31}}};
    const usize first = readU32BE(enemies, 0);
    const usize stride = readU32BE(enemies, 4);
    if (first < 8 || first > enemies.size() || stride < 52 || stride > 256) {
        throw FormatError("Invalid N64 enemy record layout");
    }
    for (usize i = 0, at = first; i < 256; ++i, at += stride) {
        if (at > enemies.size() || enemies.size() - at < 4) {
            throw FormatError("Unterminated N64 enemy records");
        }
        const u32 key = readU32BE(enemies, at);
        if (key == 0) {
            return result;
        }
        if (stride > enemies.size() - at) {
            throw FormatError("Truncated N64 enemy record");
        }
        // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
        // NOLINTNEXTLINE(readability-qualified-auto)
        const auto found = std::ranges::find(kKinds, key, &std::pair<u32, usize>::first);
        if (found == kKinds.end()) {
            continue;
        }
        const u8 family = enemies[at + 4 + 39];
        if (family >= result.m_cues.size() || result.m_families[found->second] != -1) {
            throw FormatError("Invalid or duplicate N64 enemy sound family");
        }
        result.m_families[found->second] = static_cast<s8>(family);
    }
    throw FormatError("Excessive N64 enemy records");
}

std::optional<u32> LegendsEnemySounds::feedback(const EnemyFeedback& event, f32 health) const {
    if (event.kind < 0 || static_cast<usize>(event.kind) >= m_families.size() || event.tier < 1 ||
        !std::isfinite(health)) {
        return {};
    }
    const s8 family = m_families[static_cast<usize>(event.kind)];
    if (family < 0) {
        return {};
    }
    usize index = 0;
    if (event.killed) {
        index = 6 + static_cast<usize>(std::min(event.tier - 1, 2));
    } else if (event.tier == 2) {
        index = health > 7 ? 2 : 1;
    } else if (event.tier >= 3) {
        index = health > 10 ? 5 : health > 5 ? 4 : 3;
    }
    const auto cue = m_cues[static_cast<u8>(family)][6 + index];
    return cue == 0xffffU ? std::nullopt : std::optional<u32>{cue};
}

std::optional<u32> LegendsEnemySounds::melee(s32 kind, s32 tier) const {
    if (kind < 0 || static_cast<usize>(kind) >= m_families.size() || tier < 1 || tier > 6) {
        return {};
    }
    const s8 family = m_families[static_cast<usize>(kind)];
    if (family < 0) {
        return {};
    }
    // e005cf6c/e005d134: family * 30 + (tier - 1) * 2, after player damage.
    const auto cue = m_cues[static_cast<u8>(family)][static_cast<usize>(tier - 1)];
    return cue == 0xffffU ? std::nullopt : std::optional<u32>{cue};
}
} // namespace gdl::game
