#include "game/level/BossKinds.h"

#include <array>

namespace gdl::game {
namespace {

/** One row a boss, in code order. Retail arena callback 0x80063c58 sets tree flag 1 on the
 * hidden object only; the renderer (0x800c7a70) skips its mesh but still visits its children. */
constexpr std::array<BossStage, kBossTypeCount> kStages{{
    {.kind = boss_type::kDragon, .name = "DRAGON", .fireHoleSound = "S_FIREHOLE2"},
    {.kind = boss_type::kChimera, .name = "CHIMERA"},
    {.kind = boss_type::kDjinn, .name = "DJINN", .swarmBankVariant = 'C'},
    {.kind = boss_type::kDrider, .name = "DRIDER", .swarmBankVariant = 'D', .roams = true},
    {.kind = boss_type::kPlagueBoss, .name = "PBOSS"},
    {.kind = boss_type::kYeti, .name = "YETI"},
    {.kind = boss_type::kWraith, .name = "WRAITH"},
    {.kind = boss_type::kLich,
     .name = "LICH",
     .hiddenObject = "G5BIGDIRT",
     .swarmBankVariant = 'B',
     .roams = true},
    {.kind = boss_type::kSkorne1, .name = "SKORNE1", .fallingMasonry = true},
    {.kind = boss_type::kSkorne2, .name = "SKORNE2"},
    {.kind = boss_type::kGarm, .name = "GARM", .hiddenObject = "H4NSFFXL_PURPLE"},
}};
static_assert(
    [] {
        for (usize i = 0; i < kStages.size(); ++i) {
            if (kStages[i].kind != kFirstBossType + static_cast<s32>(i)) {
                return false;
            }
        }
        return true;
    }(),
    "each boss's stage sits at its code's row");

} // namespace

const BossStage* bossStageOf(s32 code) {
    if (!isBossType(code)) {
        return nullptr;
    }
    return &kStages[static_cast<usize>(code - kFirstBossType)];
}

const BossStage* bossStageNamed(std::string_view name) {
    for (const BossStage& stage : kStages) {
        if (stage.name == name) {
            return &stage;
        }
    }
    return nullptr;
}

std::string_view bossNameOf(s32 code) {
    const BossStage* stage = bossStageOf(code);
    return stage != nullptr ? stage->name : std::string_view{};
}

std::string_view bossArenaObject(s32 code) {
    const BossStage* stage = bossStageOf(code);
    return stage != nullptr ? stage->hiddenObject : std::string_view{};
}

} // namespace gdl::game
