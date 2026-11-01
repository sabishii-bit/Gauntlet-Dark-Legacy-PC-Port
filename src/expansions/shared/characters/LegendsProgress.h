#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {

enum class LegendsPlatform : u8 { N64 = 0, Dreamcast = 1 };
inline constexpr usize kLegendsPlatformCount = 2;

enum class LegendsRelic : u8 { Runestone, Shard, Obelisk, LegendItem };

/** A native Legends campaign's collection and completion history. Indices are
 * zero-based native identities, not GDL tower realm/rune indices. */
struct LegendsProgress {
    // Storage bounds, not claims about either game's retail collectible count.
    static constexpr s32 kMaxRelics = 64;
    static constexpr usize kMaxCompletedMaps = 128;
    static constexpr usize kMaxMapName = 32;

    u64 runestones = 0;
    u64 shards = 0;
    u64 obelisks = 0;
    u64 legendItems = 0;
    std::vector<std::string> completedMaps; ///< sorted, unique, canonical native map IDs

    bool operator==(const LegendsProgress&) const = default;
    bool valid() const;
    bool has(LegendsRelic kind, s32 index) const;
    /** False for an unknown kind/index or an already-held collectible. */
    bool add(LegendsRelic kind, s32 index);
    bool hasCompleted(std::string_view map) const;
    /** False for an invalid ID, an existing completion, or a full record. */
    bool recordCompleted(std::string_view map);

    /** Case-normalizes an ASCII asset ID. Paths and punctuation other than '_'
     * and '-' are rejected; these values are never filesystem paths. */
    static std::optional<std::string> mapIdentity(std::string_view map);
};

void visitNativeState(NativeStateDigest& digest, const LegendsProgress& value);

} // namespace gdl::game
