#pragma once
#include <cstddef>
#include <functional>
#include <string_view>
#include <vector>

#include "game/players/ClassData.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/TurboMeter.h"
namespace gdl::game {
/** A character's data-driven attack timeline. Owns pending strikes, delayed payment,
 * visibility windows and volley progress; knows nothing about a scene, renderer or audio.
 * Events run synchronously, with payment settled before the first damaging strike. */
class TurboMove {
public:
    struct Events {
        std::function<void(int)> announce;
        std::function<void(float)> dim;
        std::function<void(const Vec3&)> volley;
        std::function<void(int)> strike;
    };
    /** Returns a fallback cry when a turbo attack has no rows and is paid immediately. */
    std::string_view begin(PlayerAnimator::Action action, const ClassStats* known,
                           TurboMeter& meter);
    void advance(PlayerAnimator::Action action, float frame, const Vec3& facing,
                 const ClassStats* stats, TurboMeter& meter, const Events& events);
    bool weaponHidden() const { return m_weaponHidden; }
    float owed() const { return m_owed; }

private:
    void volley(std::size_t slot, const MoveStrike& strike, float frame, const Vec3& facing,
                const Events& events);
    std::vector<int> m_pending;
    std::vector<int> m_all;
    float m_owed = 0;
    bool m_named = false;
    bool m_weaponHidden = false;
    std::vector<int> m_volleysShot;
};
} // namespace gdl::game
