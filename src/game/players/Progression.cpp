#include "game/players/Progression.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl::game {

s32 ClassProgress::appearanceLevel() const {
    return std::clamp(promotedLevel < 0 ? experienceLevel(experience) : promotedLevel, 1,
                      kMaxLevel);
}

bool ClassProgress::promotionPending() const {
    const s32 level = experienceLevel(experience);
    return level / 10 > appearanceLevel() / 10 ||
           (level == kMaxLevel && appearanceLevel() < kMaxLevel);
}

namespace {

constexpr s32 kCurveTopLevel = 60;
constexpr s32 kCurveSlope = 30;
constexpr s32 kCurveBase = 1000;
constexpr s32 kLateLevelStep = 4600;
constexpr s32 kLateLevelBase = 0x28550;
constexpr s32 kStatPerLevel = 5;

} // namespace

s32 levelExperience(s32 level) {
    if (level <= kCurveTopLevel) {
        return (level - 1) * (level * kCurveSlope + kCurveBase);
    }
    return kLateLevelBase + (level - kCurveTopLevel) * kLateLevelStep;
}

s32 experienceLevel(s32 experience) {
    for (s32 level = kMaxLevel; level > 0; --level) {
        if (experience >= levelExperience(level)) {
            return level;
        }
    }
    return 1;
}

usize StatBlock::best() const {
    usize best = 0;
    s32 top = 0;
    for (usize i = 0; i < kCount; ++i) {
        if (values[i] > top) {
            top = values[i];
            best = i;
        }
    }
    return best;
}

StatBlock displayStats(const ClassStats& stats, s32 level, const ClassProgress& progress) {
    const auto growth = static_cast<f32>((level - 1) * kStatPerLevel);
    StatBlock block;
    block.values[0] = static_cast<s32>(progress.fightAdd + stats.fightMin + growth);
    block.values[1] = static_cast<s32>(progress.speedAdd + stats.speedMin + growth);
    block.values[2] = static_cast<s32>(progress.armorAdd + stats.armorMin + growth);
    block.values[3] = static_cast<s32>(progress.magicAdd + stats.magicMin + growth);
    for (s32& value : block.values) {
        value = std::min(value, kMaxStat);
    }
    return block;
}

StatBlock masteryStats() {
    StatBlock block;
    block.values.fill(kMaxStat);
    return block;
}

f32 armorDefense(const ClassStats& stats, const ClassProgress& progress) {
    constexpr f32 kArmorPerAttribute = 0.005f;
    const f32 growth = static_cast<f32>((experienceLevel(progress.experience) - 1) * kStatPerLevel);
    const f32 base = std::min(stats.armorMin + growth, stats.armorMax);
    const f32 attribute = std::clamp(base + progress.armorAdd, 0.0f, static_cast<f32>(kMaxStat));
    return attribute * kArmorPerAttribute;
}

} // namespace gdl::game
