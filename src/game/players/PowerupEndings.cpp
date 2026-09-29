#include "game/players/PowerupEndings.h"

#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/players/PowerupEffects.h"

namespace gdl::game {

std::vector<std::string_view> PowerupEndings::soundsOf(u32 before, u32 after, bool plainSize) {
    const u32 lost = before & ~after;
    std::vector<std::string_view> sounds;
    // In the order PlayerProcessPowerups reaches them: Pojo's leaving, the body's size, then
    // the wings folding.
    if ((lost & powerup::kPojo) != 0) {
        sounds.push_back(kUnpojo);
    }
    if ((lost & powerup::kGrowth) != 0 && plainSize) {
        sounds.push_back(kUngrow);
    }
    if ((lost & powerup::kLevitation) != 0) {
        sounds.push_back(kLevitationDown);
    }
    return sounds;
}

} // namespace gdl::game
