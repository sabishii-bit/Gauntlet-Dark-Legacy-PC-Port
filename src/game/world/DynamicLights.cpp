#include "game/world/DynamicLights.h"

#include <algorithm>
#include <array>

namespace gdl::game {
namespace {
constexpr std::array<Vec3, 7> kColors{Vec3{2, 0, 0}, Vec3{0, 2, 0}, Vec3{0, 0, 2}, Vec3{2, 2, 0},
                                      Vec3{2, 0, 2}, Vec3{0, 2, 2}, Vec3{1, 1, 1}};
constexpr std::array<usize, 4> kCostume{3, 2, 0, 1};   ///< pclr_idx: by costume colour
constexpr std::array<usize, 5> kDamage{6, 0, 6, 3, 1}; ///< mclr_idx: by damage type
constexpr std::array<Vec3, 4> kLanterns{Vec3{2.0f, 2.0f, 1.5f}, Vec3{1.5f, 1.5f, 2.0f},
                                        Vec3{2.0f, 1.5f, 1.5f}, Vec3{1.5f, 2.0f, 1.5f}};
constexpr std::array<Vec3, 4> kClasses{Vec3{2, 0, 0}, Vec3{0, 0, 2}, Vec3{0, 2, 2}, Vec3{0, 2, 0}};
constexpr s32 kFireType = 1;
constexpr s32 kLastType = 4;

template <typename T> usize clampedIndex(s32 index, const T& table) {
    return static_cast<usize>(std::clamp(index, 0, static_cast<s32>(table.size()) - 1));
}
} // namespace

Vec3 DynamicLights::ofCostume(s32 color) {
    return kColors[kCostume[clampedIndex(color, kCostume)]];
}

Vec3 DynamicLights::ofDamage(s32 type) {
    return kColors[kDamage[clampedIndex(type, kDamage)]];
}

Vec3 DynamicLights::ofPotion(s32 kind) {
    return ofDamage(std::clamp(kind, kFireType, kLastType));
}

Vec3 DynamicLights::ofClass(s32 character) {
    return kClasses[static_cast<usize>(std::max(character, 0)) % kClasses.size()];
}

Vec3 DynamicLights::lantern(s32 color) {
    return kLanterns[clampedIndex(color, kLanterns)];
}

} // namespace gdl::game
