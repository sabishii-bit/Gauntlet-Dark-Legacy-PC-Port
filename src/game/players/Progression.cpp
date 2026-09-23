#include "game/players/Progression.h"

#include <algorithm>
#include <cstddef>

namespace gdl::game {

namespace {

constexpr int kCurveTopLevel = 60;
constexpr int kCurveSlope = 30;
constexpr int kCurveBase = 1000;
constexpr int kLateLevelStep = 4600;
constexpr int kLateLevelBase = 0x28550;
constexpr int kStatPerLevel = 5;

} // namespace

int levelExperience(int level) {
    if (level <= kCurveTopLevel) {
        return (level - 1) * (level * kCurveSlope + kCurveBase);
    }
    return kLateLevelBase + (level - kCurveTopLevel) * kLateLevelStep;
}

int experienceLevel(int experience) {
    for (int level = kMaxLevel; level > 0; --level) {
        if (experience >= levelExperience(level)) {
            return level;
        }
    }
    return 1;
}

std::size_t StatBlock::best() const {
    std::size_t best = 0;
    int top = 0;
    for (std::size_t i = 0; i < kCount; ++i) {
        if (values[i] > top) {
            top = values[i];
            best = i;
        }
    }
    return best;
}

StatBlock displayStats(const ClassStats& stats, int level, const ClassProgress& progress) {
    const auto growth = static_cast<float>((level - 1) * kStatPerLevel);
    StatBlock block;
    block.values[0] = static_cast<int>(progress.fightAdd + stats.fightMin + growth);
    block.values[1] = static_cast<int>(progress.speedAdd + stats.speedMin + growth);
    block.values[2] = static_cast<int>(progress.armorAdd + stats.armorMin + growth);
    block.values[3] = static_cast<int>(progress.magicAdd + stats.magicMin + growth);
    for (int& value : block.values) {
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
