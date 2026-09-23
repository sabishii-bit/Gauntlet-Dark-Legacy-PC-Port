#include "game/enemies/SkorneRelics.h"

#include <array>
#include <string_view>

#include "engine/core/Types.h"

#include "game/enemies/BossCoins.h"

namespace gdl::game {
std::array<SkorneRelics::Drop, 4> SkorneRelics::spray(const Vec3& velocity, f32 halfAngle) {
    // BossSpewCoins' kind-42 table at 0x80118a88: right gauntlet, mask,
    // horn, left gauntlet. No player multiplier or randomized coin speed.
    constexpr std::array<std::string_view, 4> kNames{"BGNTR_IC", "BMASK_IC", "BHORN_IC",
                                                     "BGNTL_IC"};
    std::array<Drop, 4> drops{};
    const f32 step = halfAngle * 0.5f;
    f32 angle = -halfAngle + step * 0.5f;
    for (usize i = 0; i < drops.size(); ++i) {
        drops[i] = {kNames[i], BossCoins::yawed(velocity, angle)};
        angle += step;
    }
    return drops;
}
} // namespace gdl::game
