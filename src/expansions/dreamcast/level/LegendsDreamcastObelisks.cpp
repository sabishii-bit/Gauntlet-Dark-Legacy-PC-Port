#include "expansions/dreamcast/level/LegendsDreamcastObelisks.h"

#include <utility>

namespace gdl::game {

std::optional<LegendsDreamcastObelisk>
legendsDreamcastObelisk(const formats::ItemInfoRecord& info,
                        const formats::ItemInstanceRecord& instance, u32 nativeIndex,
                        std::string* diagnostic) {
    if (diagnostic)
        diagnostic->clear();
    if (info.type != 1 || info.subtype != 12)
        return {};
    auto pickup = legendsDreamcastPickup(info, instance, nativeIndex, -1, diagnostic);
    if (!pickup)
        return {};
    LegendsDreamcastObelisk result;
    result.pickup = std::move(*pickup);
    // SetItem (8c0a8340) grows the definition's horizontal bounds by 1.1. Both
    // touch and solid-body tests use those expanded half extents. Type1's
    // do_got_it branch never removes the obelisk after granting its party bit.
    result.body.centre = result.pickup.position;
    result.body.yaw = result.pickup.yaw;
    result.body.halfAcross = result.pickup.xSize;
    result.body.halfAlong = result.pickup.zSize;
    result.body.height = result.pickup.height;
    result.body.enemyItem = Obstacle::ItemQuery{false, result.pickup.radius};
    return result;
}

} // namespace gdl::game
