#include "game/players/Progression.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace gdl::game {

namespace {

constexpr std::int32_t kCurveTopLevel = 60;
constexpr std::int32_t kCurveSlope = 30;
constexpr std::int32_t kCurveBase = 1000;
constexpr std::int32_t kLateLevelStep = 4600;
constexpr std::int32_t kLateLevelBase = 0x28550;
constexpr std::int32_t kStatPerLevel = 5;

} // namespace

std::int32_t levelExperience(std::int32_t level) {
    if (level <= kCurveTopLevel) {
        return (level - 1) * (level * kCurveSlope + kCurveBase);
    }
    return kLateLevelBase + (level - kCurveTopLevel) * kLateLevelStep;
}

std::int32_t experienceLevel(std::int32_t experience) {
    for (std::int32_t level = kMaxLevel; level > 0; --level) {
        if (experience >= levelExperience(level)) {
            return level;
        }
    }
    return 1;
}

std::size_t StatBlock::best() const {
    std::size_t best = 0;
    std::int32_t top = 0;
    for (std::size_t i = 0; i < kCount; ++i) {
        if (values[i] > top) {
            top = values[i];
            best = i;
        }
    }
    return best;
}

StatBlock displayStats(const ClassStats& stats, std::int32_t level, const ClassProgress& progress) {
    const auto growth = static_cast<float>((level - 1) * kStatPerLevel);
    StatBlock block;
    block.values[0] = static_cast<std::int32_t>(progress.fightAdd + stats.fightMin + growth);
    block.values[1] = static_cast<std::int32_t>(progress.speedAdd + stats.speedMin + growth);
    block.values[2] = static_cast<std::int32_t>(progress.armorAdd + stats.armorMin + growth);
    block.values[3] = static_cast<std::int32_t>(progress.magicAdd + stats.magicMin + growth);
    for (std::int32_t& value : block.values) {
        value = std::min(value, kMaxStat);
    }
    return block;
}

StatBlock masteryStats() {
    StatBlock block;
    block.values.fill(kMaxStat);
    return block;
}

} // namespace gdl::game
