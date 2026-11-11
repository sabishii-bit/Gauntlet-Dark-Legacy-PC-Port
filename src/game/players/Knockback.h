#pragma once

#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/DamageTypes.h"

namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {

/**
 * The slide a hit leaves a character in (PlayerKnockback, PlayerMotion). What a frame's hits
 * push adds up with their flags; the next frame turns it, when more than a point of harm came
 * with it, into a kick by the heaviest flag among them, and turns the body to face along the
 * push or against it, whichever is nearer. The slide then decays by a third of itself a
 * frame. A kick never lifts; only a direct contact velocity may carry a rise.
 *
 * Apart from the slide, another member walking into the character shoves it by the step it
 * meant (the push velocity, player +0x864): what a frame's shoves add up to carries the body
 * on at most half its pace, decaying by a third a frame, and the frame after one the
 * character shows it is being pushed (hud_flags 4, then 8; do_players, player.c 2300).
 *
 * The slide and the shove go with the step's walk, and the three together are held along each
 * axis to the step's limit (PlayerMotion's moveLimit): forty a second through the native frame
 * after a fall's kick, one and a half times the character's pace otherwise.
 */
class Knockback {
    friend void visitNativeState(NativeStateDigest& /*digest*/, const Knockback& /*value*/);

public:
    bool operator==(const Knockback&) const = default;
    static constexpr u32 kKnockBack = damage::kKnockBack;
    static constexpr u32 kKnockDown = damage::kKnockDown;
    static constexpr u32 kBlownAway = damage::kBlownAway;
    static constexpr u32 kKnockOver = damage::kKnockOver;
    static constexpr u32 kWhirlwind = damage::kWhirlwind;
    /** The flags that raise the next frame's limit to kFastLimit (0x18160). */
    static constexpr u32 kFastSlide = damage::kHeavyKnocks | damage::kThrown;
    static constexpr f32 kWhirlKick = 100.0f;
    static constexpr f32 kBlownKick = 100.0f;
    static constexpr f32 kFallKick = 32.0f;
    static constexpr f32 kPojoFallKick = 80.0f; ///< when Pojo is carried
    static constexpr f32 kKnockKick = 16.0f;
    static constexpr f32 kKickFrom = 1.0f;   ///< harm a push must come with
    static constexpr f32 kDecay = 0.667f;    ///< what is left of the slide after a frame
    static constexpr f32 kFastLimit = 40.0f; ///< units a second (the double at 0x80347B90)
    static constexpr f32 kPaceLimit = 1.5f;  ///< of the character's pace (0x80347B88)
    static constexpr f32 kFrameRate = 30.0f;
    static constexpr f32 kStill = 0.01f;     ///< a slide slower than this is over
    static constexpr f32 kShoveLimit = 0.5f; ///< of the character's pace, a shove's most
    /** A slide carrying the body at least this far a native frame (0.0025 squared,
     * 0x80347B70) stops a walk more than kOpposeTurn off its way. */
    static constexpr f32 kOpposeTravel = 0.05f;
    static constexpr f32 kOpposeTurn = 2.0943951f; ///< a third of a turn (0x80347B80)

    /** Whether a slide at `velocity` stops a walk along `heading` (PlayerMotion's speed
     * scale nought, 0x800818b4-0x80081940): fast enough, and more than a third of a turn from
     * it. */
    static bool opposes(const Vec3& velocity, f32 heading);

    /** A hit's push (the way it travels, as long as its maker gave it) and its flags. */
    void queue(const Vec3& push, u32 flags, f32 damage);
    /** Turns what the last frame's hits pushed into a kick, the heaviest flag's; the heading
     * the body now faces, or nullopt when nothing kicked. */
    std::optional<f32> kick(f32 facing, bool pojo);
    /** Where the slide carries the body over `seconds` at a character's `pace`, the slide
     * decaying after: the velocity, and the shove at most half the pace, before the step's
     * limit holds the step they go with. */
    Vec3 step(f32 seconds, f32 pace);
    /** The most a step of `seconds` at `pace` may carry the body along each axis, walk, slide
     * and shove together (PlayerMotion 0x80081ac8-0x80081afc), asked before step(). */
    f32 limit(f32 seconds, f32 pace) const;
    /** Another member's step of `seconds` ran into the character: it is shoved by it, as
     * by the step a 30 Hz frame would have made. */
    void shove(const Vec3& displacement, f32 seconds);
    /** Direct contact velocity, independent of damage, reaction flags and party shoves. */
    void addVelocity(const Vec3& velocity) { m_velocity += velocity; }
    /** The frame is over: what shoved the character this frame shows it next. */
    void endFrame();
    /** Whether the character was shoved in the last frame. */
    bool pushed() const { return m_pushed; }
    void clear();

    const Vec3& velocity() const { return m_velocity; }
    bool sliding() const { return glm::length(m_velocity) > kStill; }
    bool pending() const { return m_flags != 0; }

private:
    Vec3 m_force{0.0f};
    u32 m_flags = 0;
    f32 m_damage = 0.0f;
    Vec3 m_velocity{0.0f};
    f32 m_fastSeconds = 0;  ///< what is left of the native frame held to kFastLimit
    f32 m_frameSeconds = 0; ///< time within the current 30 Hz decay frame
    Vec3 m_shove{0.0f};     ///< units a second
    bool m_shoved = false;
    bool m_pushed = false;
};

} // namespace gdl::game
