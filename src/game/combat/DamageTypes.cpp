#include "game/combat/DamageTypes.h"

#include <array>

namespace gdl::game::damage {
namespace {
constexpr std::array<std::string_view, kElementCount> kHits{"BLOODFX1", "FIREHIT", "HITCOL",
                                                            "HITCOL", "HITCOL"};
constexpr std::array<std::string_view, kElementCount> kDeaths{"BLOODFX2", "FIREDIE", "ELECDIE",
                                                              "LIGHTDIE", "ACIDDIE"};
constexpr std::array<std::string_view, kElementCount> kColours{"", "RED", "BLU", "YEL", "GRE"};
/** By the player colours' order (yellow, blue, red, green). */
constexpr std::array<u32, 4> kColourElements{kLight, kElectric, kFire, kAcid};
} // namespace

u32 element(u32 flags) {
    return flags & kElementMask;
}

bool heals(u32 flags) {
    return (flags & kHeal) != 0;
}

bool marks(u32 flags) {
    return (flags & kNoHitEffect) == 0;
}

std::string_view hitEffect(u32 element, bool killed) {
    if (element >= kElementCount) {
        return {};
    }
    return killed ? kDeaths[element] : kHits[element];
}

std::string_view colourOf(u32 element) {
    return element < kElementCount ? kColours[element] : std::string_view{};
}

u32 elementOfColour(s32 color) {
    if (color < 0 || static_cast<usize>(color) >= kColourElements.size()) {
        return 0;
    }
    return kColourElements[static_cast<usize>(color)];
}

} // namespace gdl::game::damage
