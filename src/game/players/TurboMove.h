#pragma once
#include <functional>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/characters/ClassData.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/TurboMeter.h"
namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {
/** A character's data-driven attack timeline. Owns pending strikes, delayed payment,
 * visibility windows and volley progress; knows nothing about a scene, renderer or audio.
 * Events run synchronously, with payment settled before the first damaging strike. */
class TurboMove {
    friend void visitNativeState(NativeStateDigest& /*digest*/, const TurboMove& /*value*/);

public:
    bool operator==(const TurboMove&) const = default;
    static constexpr usize kMaxCheckpointRows = 128;
    static constexpr s32 kMaxCheckpointVolleys = 65536;

    struct Volley {
        Vec3 direction{0};
        usize slot = 0; // occurrence in the retained chain, not the authored row ID
        s32 index = 0;  // counter before this emission, including skipped-frame catch-up
        /** Zero means outside the bounded replay domain. The action generation
         * and seat/epoch are supplied by the caller, not this native timeline. */
        u32 emissionIdentity() const;
        bool operator==(const Volley&) const = default;
    };
    struct Strike {
        s32 row = -1;
        usize slot = 0; ///< occurrence in the complete retained chain, not the pending list
        bool operator==(const Strike&) const = default;
    };
    struct Events {
        std::function<void(s32)> announce;
        std::function<void(f32)> dim;
        std::function<void(const Vec3&)> volley;
        std::function<void(s32)> strike;
        /** When supplied, replaces volley rather than dispatching the same shot twice. */
        std::function<void(const Volley&)> identifiedVolley;
        /** When supplied, replaces strike, after the same native payment and volley phases. */
        std::function<void(const Strike&)> identifiedStrike;
    };
    /** Returns a fallback cry when a turbo attack has no rows and is paid immediately. */
    std::string_view begin(PlayerAnimator::Action action, const ClassStats* known,
                           TurboMeter& meter);
    void advance(PlayerAnimator::Action action, f32 frame, const Vec3& facing,
                 const ClassStats* stats, TurboMeter& meter, const Events& events);
    bool weaponHidden() const { return m_weaponHidden; }
    f32 owed() const { return m_owed; }

private:
    void volley(usize slot, const MoveStrike& strike, f32 frame, const Vec3& facing,
                const Events& events);
    std::vector<s32> m_pending;
    std::vector<s32> m_all;
    f32 m_owed = 0;
    bool m_named = false;
    bool m_weaponHidden = false;
    std::vector<s32> m_volleysShot;
};
} // namespace gdl::game
