#include "expansions/dreamcast/level/LegendsDreamcastHazards.h"

#include "engine/core/Error.h"

namespace gdl::game {

std::vector<u32> dreamcastSurfaceHazardFlags(const formats::LegendsMap& map) {
    // WorldObjGetAllFlags8c0e8b60 ORs the owner and every parent. Player
    // FloorFX8c0c4f00 and EnemyWorldDamage8c099860 share these masks and the
    // common HazardSurfaces damage/impact table. Preserve unknown harm kinds:
    // the player defaults to 5 damage, while enemies ignore those kinds.
    constexpr u32 kContactFlags = 0x0A0F0000;
    constexpr usize kMaximumObjects = 16384;
    if (map.objects.size() > kMaximumObjects)
        throw FormatError("Dreamcast surface hazard object budget exceeded");

    std::vector<u32> result(map.objects.size());
    std::vector<u8> state(map.objects.size());
    std::vector<usize> path;
    for (usize start = 0; start < map.objects.size(); ++start) {
        if (state[start] == 2)
            continue;
        path.clear();
        s32 cursor = static_cast<s32>(start);
        u32 inherited = 0;
        while (cursor != -1) {
            if (cursor < 0 || static_cast<usize>(cursor) >= map.objects.size())
                throw FormatError("Dreamcast surface hazard parent is out of range");
            const auto index = static_cast<usize>(cursor);
            if (state[index] == 1)
                throw FormatError("Dreamcast surface hazard hierarchy is cyclic");
            if (state[index] == 2) {
                inherited = result[index];
                break;
            }
            state[index] = 1;
            path.push_back(index);
            cursor = map.objects[index].parent;
        }
        for (auto at = path.rbegin(); at != path.rend(); ++at) {
            inherited |= map.objects[*at].flags & kContactFlags;
            result[*at] = inherited;
            state[*at] = 2;
        }
    }
    return result;
}

} // namespace gdl::game
