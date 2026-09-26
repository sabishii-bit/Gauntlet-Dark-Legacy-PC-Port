#pragma once

#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {
/** Death's two forms consume different resources, independently of ordinary melee. */
enum class DeathForm : u8 { Red, Black };

struct DeathRules {
    static constexpr s32 kDrainTicks = 3;
    static constexpr f32 kFadeSeconds = 255.0f / 240.0f;
    static constexpr f32 kRiseSpeed = 10.0f;
    static constexpr u32 kProtection = 0x80000;
    static DeathForm form(s32 tier);
    static std::string_view effect(DeathForm form);
    static s32 experience(s32 level, bool stealing);
    static f32 magicHealing(s32 level, f32 remainingHealth);
};

/** Resource transfers and presentation cues emitted by a Death, not ordinary hit rewards. */
struct DeathEvent {
    enum class Kind : u8 { Drain, Return, MagicHeal, Killed, Exhausted, Awakened };
    Kind kind = Kind::Drain;
    s32 enemy = -1;
    s32 player = -1;
    DeathForm form = DeathForm::Red;
    f32 amount = 0;
    Vec3 position{0};
};
} // namespace gdl::game
