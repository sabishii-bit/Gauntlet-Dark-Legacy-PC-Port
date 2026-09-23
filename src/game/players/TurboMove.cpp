#include "game/players/TurboMove.h"

#include <algorithm>
#include <cmath>
namespace gdl::game {
namespace {
constexpr f32 kMoveNamedFrame = 1.0f;
}
std::string_view TurboMove::begin(PlayerAnimator::Action action, const ClassStats* known,
                                  TurboMeter& meter) {
    m_pending.clear();
    m_all.clear();
    m_owed = 0.0f;
    m_named = false;
    const bool full = action == PlayerAnimator::Action::TurboFull;
    m_weaponHidden = false;
    m_volleysShot.clear();
    if (action == PlayerAnimator::Action::StrongThrow) {
        // The strong throw costs nothing; its strikes are only what it shows and sounds.
        if (known != nullptr) {
            m_pending = known->strikesOf(known->moves.turboAThrow);
            m_all = m_pending;
            m_volleysShot.assign(m_all.size(), 0);
        }
        return {};
    }
    if (!full && action != PlayerAnimator::Action::TurboStrong) {
        return {};
    }
    m_owed = full ? TurboMeter::kFullCost : TurboMeter::kStrongCost;
    if (const ClassStats* stats = known) {
        if (full) {
            m_pending = stats->strikesOf(stats->moves.turboC1);
            const std::vector<s32> second = stats->strikesOf(stats->moves.turboC2);
            m_pending.insert(m_pending.end(), second.begin(), second.end());
        } else {
            m_pending = stats->strikesOf(stats->moves.turboB);
        }
    }
    m_all = m_pending;
    m_volleysShot.assign(m_all.size(), 0);
    if (m_pending.empty()) {
        meter.spend(m_owed);
        m_owed = 0.0f;
        return full ? "TURBOC" : "TURBOB";
    }
    return {};
}

void TurboMove::advance(PlayerAnimator::Action action, f32 frame, const Vec3& facing,
                        const ClassStats* stats, TurboMeter& meter, const Events& events) {
    const bool attacking = action == PlayerAnimator::Action::TurboFull ||
                           action == PlayerAnimator::Action::TurboStrong ||
                           action == PlayerAnimator::Action::StrongThrow;
    if (!attacking) {
        m_pending.clear();
        m_all.clear();
        m_owed = 0.0f;
        m_weaponHidden = false;
        return;
    }
    if (stats == nullptr) {
        return;
    }
    // A frame in, the move is named.
    if (!m_named && frame >= kMoveNamedFrame) {
        m_named = true;
        for (const s32 strike : m_all) {
            if (const s32 help = stats->moveStrikes[static_cast<usize>(strike)].help; help >= 0) {
                events.announce(help);
                break;
            }
        }
    }
    // The level goes dark for as long as a strike that darkens it lasts; the hand is empty
    // for as long as one that hides the weapon does; a volley lets fly as it lasts.
    m_weaponHidden = false;
    for (usize slot = 0; slot < m_all.size(); ++slot) {
        const MoveStrike& row = stats->moveStrikes[static_cast<usize>(m_all[slot])];
        if (!row.lasting(frame)) {
            continue;
        }
        if (row.dimming() != 0.0f) {
            events.dim(row.dimming());
        }
        if ((row.flags & MoveStrike::kHidesWeapon) != 0) {
            m_weaponHidden = true;
        }
        if (row.type == MoveStrike::kVolley) {
            volley(slot, row, frame, facing, events);
        }
    }
    std::vector<s32> due;
    std::erase_if(m_pending, [&](s32 strike) {
        const bool now =
            frame >= static_cast<f32>(stats->moveStrikes[static_cast<usize>(strike)].startFrame);
        if (now) {
            due.push_back(strike);
        }
        return now;
    });
    for (const s32 strike : due) {
        if (stats->moveStrikes[static_cast<usize>(strike)].amount != 0.0f && m_owed > 0.0f) {
            meter.spend(m_owed);
            m_owed = 0.0f;
        }
        events.strike(strike);
    }
}

void TurboMove::volley(usize slot, const MoveStrike& strike, f32 frame, const Vec3& facing,
                       const Events& events) {
    if (slot >= m_volleysShot.size()) {
        return;
    }
    const f32 since = frame - static_cast<f32>(strike.startFrame);
    const f32 every = strike.delay > 0.0f ? strike.delay : 1.0e6f; // none: one shot only
    const auto due = static_cast<s32>(std::floor(since / every)) + 1;
    const f32 span = static_cast<f32>(strike.endFrame - strike.startFrame);
    while (m_volleysShot[slot] < due) {
        const f32 shotAt = static_cast<f32>(m_volleysShot[slot]) * every;
        f32 angle = strike.angle;
        if ((strike.flags & (MoveStrike::kSweepsIn | MoveStrike::kSweepsOut)) != 0) {
            f32 through = span > 0.0f ? std::clamp(shotAt / span, 0.0f, 1.0f) : 1.0f;
            if ((strike.flags & MoveStrike::kSweepsIn) != 0) {
                through = 1.0f - through;
            }
            angle *= through;
        }
        const f32 heading = std::atan2(facing.x, facing.z) + angle;
        events.volley(Vec3{std::sin(heading), 0.0f, std::cos(heading)});
        ++m_volleysShot[slot];
    }
}
} // namespace gdl::game
