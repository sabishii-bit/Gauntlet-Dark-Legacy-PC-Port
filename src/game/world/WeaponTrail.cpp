#include "game/world/WeaponTrail.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl::game {

void WeaponTrail::step(s32 ticks, const Mat4& weapon, bool swinging) {
    if (ticks <= 0) {
        return;
    }
    Ghost* newest = nullptr;
    Ghost* free = nullptr;
    Ghost* faintest = nullptr;
    for (Ghost& ghost : m_ghosts) {
        if (!ghost.shown) {
            free = free != nullptr ? free : &ghost;
            continue;
        }
        ghost.fade = std::min(ghost.fade + kFadePerTick * ticks, kClear);
        if (ghost.fade >= kClear) {
            ghost = Ghost{};
            free = free != nullptr ? free : &ghost;
            continue;
        }
        if (newest == nullptr || ghost.fade < newest->fade) {
            newest = &ghost;
        }
        if (faintest == nullptr || ghost.fade > faintest->fade) {
            faintest = &ghost;
        }
    }
    if (!swinging) {
        return;
    }
    const Vec3 at{weapon[3]};
    if (newest != nullptr && glm::distance(Vec3{newest->placement[3]}, at) < kLeastStep) {
        return;
    }
    Ghost* slot = free != nullptr ? free : faintest;
    *slot = Ghost{weapon, 0, true};
}

usize WeaponTrail::count() const {
    return static_cast<usize>(
        std::ranges::count_if(m_ghosts, [](const Ghost& g) { return g.shown; }));
}

} // namespace gdl::game
