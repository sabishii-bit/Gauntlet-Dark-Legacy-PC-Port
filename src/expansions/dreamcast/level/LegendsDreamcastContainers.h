#pragma once

#include <optional>
#include <span>
#include <vector>

#include "expansions/n64/level/LegendsContainers.h"
#include "formats/WorldFile.h"

namespace gdl::game {

/** Type2 loot containers and type10 empty/fire/gas barrels. Scripted/hidden
 * placements are not silently converted into plain breakable barrels. */
std::optional<LegendsContainerDefinition>
legendsDreamcastContainer(const formats::ItemInfoRecord& info,
                          const formats::ItemInstanceRecord& instance, u32 nativeIndex,
                          std::string* diagnostic = nullptr);

/** Dreamcast choice rows form a graph of native definition indices. All
 * reachable leaves can be bound before play; selection consumes the shared
 * native cursor only when the container actually opens (8c0ae4e0). */
class LegendsDreamcastLoot {
public:
    LegendsDreamcastLoot() = default;
    /** Offline scene seed, supplied explicitly when reproducing a native run.
     * Zero is our reproducible default, not the retail level loader's seed. */
    explicit LegendsDreamcastLoot(std::span<const formats::ItemInfoRecord> definitions,
                                  u32 seed = 0);
    const formats::ItemInfoRecord& definition(s16 index) const;
    std::vector<s16> outcomes(s16 root) const;
    std::optional<s16> select(s16 root, u32 instanceIndex);
    void advance(f32 seconds);
    u32 seed() const { return m_seed; }

private:
    std::vector<formats::ItemInfoRecord> m_definitions;
    u32 m_seed = 0;
    f64 m_fraction = 0;
};

} // namespace gdl::game
