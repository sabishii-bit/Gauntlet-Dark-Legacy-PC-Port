#pragma once

#include <numbers>
#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** One player's part in a two-player combo. */
enum class ComboRole : u8 {
    None,
    Grabber, ///< the one who started it, playing its class's COMBOACT sequences
    Held,    ///< the partner, in the grabber's hands (hud_flags 0x10)
    Thrown   ///< the partner let go of: a warrior's pinball or a dwarf's charger (0x40)
};

/** One player's side of a two-player combo, as the original keeps it on the player
 * (grab_partner, hud_flags 0x10/0x40/0x80, timer_1FA/1FC, saved_pos). */
struct ComboState {
    ComboRole role = ComboRole::None;
    s32 partner = -1;             ///< the other's party index
    s32 grabberClass = -1;        ///< the class making the combo: the held one plays its COMBO<CLS>
    bool riding = false;          ///< this body hangs on the other's DUMMY node (hud_flags 0x20)
    bool rideAsked = false;       ///< the dwarf keeps COMBOACT2 up while its ride lasts (0x80)
    s32 ticksLeft = 0;            ///< of a flight or a ride (timer_1FA)
    s32 turnTicks = 0;            ///< before a flying body may turn again (timer_1FC)
    f32 graceSeconds = 0.0f;      ///< while a flier still passes through its thrower (combo_fade)
    Vec3 saved{0.0f, 0.0f, 0.0f}; ///< where it stood when it was taken up (saved_pos)

    bool active() const { return role != ComboRole::None; }
};

/** What the grabber's body is doing this tick. */
struct ComboPhase {
    bool act1 = false; ///< COMBOACT1 is playing
    bool act2 = false; ///< COMBOACT2 is
    f32 frame = 0.0f;  ///< of the sequence
};

/** What one tick of the combo asks of the two bodies (PlayerMotion, pmotion.c 2960). */
struct ComboOrders {
    enum class Attach : u8 { None, PartnerOnGrabber, GrabberOnPartner };
    Attach attach = Attach::None;
    bool turnPartnerAway = false; ///< the partner turns its back on the grabber
    bool releasePartner = false;  ///< let go of where the node left it, to fly or be ridden
    bool restorePartner = false;  ///< set back where it stood when it was taken up
    bool restoreGrabber = false;
    bool detachGrabber = false; ///< the dwarf gets off where it hangs
    bool unlink = false;        ///< the combo is over for both
};

/**
 * The two-player combo of the original (PlayerMotion, fn_80088938, fn_80088EF4): with the
 * combo button held and half the turbo meter, a character takes hold of another standing
 * within five ahead of it and the pair play the grabber's class move. A warrior lifts its
 * partner and at the thirtieth frame lets it fly as a pinball, thirty a second along its
 * facing for four seconds, turning three eighths round at whatever it strikes; a dwarf
 * climbs on its partner's back and steers it, half again as fast, for four seconds; a
 * valkyrie or an archer is lifted by its partner for its move; a wizard, knight or sorceress
 * lifts its partner for theirs; a jester holds its partner for a frame. The bodies ride the
 * other's DUMMY node meanwhile. Nothing here knows a scene, a figure or a sound.
 */
class ComboMove {
public:
    static constexpr f32 kReach = 5.0f;        ///< how far ahead a partner is looked for
    static constexpr f32 kCone = 0.707f;       ///< the least cosine off the facing
    static constexpr f32 kRise = 3.0f;         ///< no higher or lower than this
    static constexpr f32 kMeterNeeded = 50.0f; ///< of the turbo meter, to begin
    static constexpr f32 kCost = 50.0f;        ///< what it takes (coll_score)
    static constexpr f32 kThrowFrame = 30.0f;  ///< of COMBOACT1: the warrior lets go
    static constexpr s32 kFlightTicks = 240;   ///< a flight or a ride (timer_1FA)
    static constexpr s32 kTurnGap = 10;        ///< ticks between a flier's turns (timer_1FC)
    static constexpr f32 kFlightSpeed = 30.0f; ///< units a second, along the facing
    static constexpr f32 kBounceTurn = 3.0f * std::numbers::pi_v<f32> / 4.0f;
    static constexpr f32 kThrowerGrace = 1.5f; ///< seconds a flier passes through its thrower
    static constexpr f32 kWarriorBlow = 50.0f; ///< what a pinball does to what it hits
    static constexpr f32 kDwarfBlow = 10.0f;   ///< and a charger
    static constexpr f32 kRidePace = 1.5f;     ///< a charger's pace, steered by the dwarf
    static constexpr f32 kRideTurn = 0.5f;     ///< and how far it turns a frame
    static constexpr s32 kWarrior = 0;
    static constexpr s32 kValkyrie = 1;
    static constexpr s32 kWizard = 2;
    static constexpr s32 kArcher = 3;
    static constexpr s32 kDwarf = 4;
    static constexpr s32 kKnight = 5;
    static constexpr s32 kSorceress = 6;
    static constexpr s32 kJester = 7;

    /** Someone who might be taken hold of: where they stand, and whether they may be. */
    struct Candidate {
        Vec3 position{0.0f, 0.0f, 0.0f};
        bool eligible = false;
    };

    /** The nearest candidate but `self` within reach, no higher or lower than the rise, in
     * the cone ahead of `facing` from `position` (fn_80088EF4); none when nobody is. */
    static std::optional<usize> findPartner(usize self, const Vec3& position, const Vec3& facing,
                                            std::span<const Candidate> candidates);

    /** Ties the two together as the grabber's move begins. */
    static void link(ComboState& grabber, usize grabberIndex, ComboState& partner,
                     usize partnerIndex, s32 grabberClass);

    /** One tick of the grabber's move, `ticks` on: what the bodies are to do, the states kept
     * up to date (attachments, roles, timers) before it is returned. */
    static ComboOrders advance(ComboState& grabber, ComboState& partner, const ComboPhase& phase,
                               s32 ticks);

    /** Which class's flight the thrown partner is on: the warrior's pinball or the dwarf's
     * charger; none for the rest, whose partners are only held. */
    static bool flies(const ComboState& state) {
        return state.role == ComboRole::Thrown && state.grabberClass == kWarrior;
    }
    static bool ridden(const ComboState& state) {
        return state.role == ComboRole::Thrown && state.grabberClass == kDwarf;
    }

    /** A flier turned three eighths round by what it ran into (hitKind 1). */
    static f32 bounceYaw(f32 yaw);
    /** A flier's facing reflected off a wall whose normal is `normal` (hitKind 2). */
    static f32 reflectedYaw(f32 yaw, const Vec3& normal);
    /** Whether a flier that hit something turns now: once, then not for ten ticks. */
    static bool takeTurn(ComboState& flier);
    /** Runs the flier's clocks: the turn gap and the grace through its thrower. */
    static void tick(ComboState& flier, s32 ticks, f32 seconds);
    static void clear(ComboState& state) { state = ComboState{}; }
};

} // namespace gdl::game
