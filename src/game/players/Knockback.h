#pragma once

#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/**
 * The slide a hit leaves a character in (PlayerKnockback, PlayerMotion). What a frame's hits
 * push adds up with their flags; the next frame turns it, when more than a point of harm came
 * with it, into a kick by the heaviest flag among them, and turns the body to face along the
 * push or against it, whichever is nearer. The slide then decays by a third of itself a
 * frame, travelling at most forty a second the frame after a fall's kick and one and a half
 * times the character's pace otherwise. It is along the ground only.
 *
 * Apart from the slide, another member walking into the character shoves it by the step it
 * meant (the push velocity, player +0x864): what a frame's shoves add up to carries the body
 * on at most half its pace, decaying by a third a frame, and the frame after one the
 * character shows it is being pushed (hud_flags 4, then 8; do_players, player.c 2300).
 */
class Knockback {
public:
    static constexpr u32 kKnockBack = 0x10;
    static constexpr u32 kKnockDown = 0x20;
    static constexpr u32 kBlownAway = 0x40;
    static constexpr u32 kKnockOver = 0x100;
    static constexpr u32 kWhirlwind = 0x10000;
    static constexpr u32 kFastSlide = 0x18160; ///< the flags that free the first frame's travel
    static constexpr f32 kWhirlKick = 100.0f;
    static constexpr f32 kBlownKick = 100.0f;
    static constexpr f32 kFallKick = 32.0f;
    static constexpr f32 kPojoFallKick = 80.0f; ///< when Pojo is carried
    static constexpr f32 kKnockKick = 16.0f;
    static constexpr f32 kKickFrom = 1.0f;   ///< harm a push must come with
    static constexpr f32 kDecay = 0.667f;    ///< what is left of the slide after a frame
    static constexpr f32 kFastLimit = 40.0f; ///< units a second
    static constexpr f32 kPaceLimit = 1.5f;  ///< of the character's pace
    static constexpr f32 kFrameRate = 30.0f;
    static constexpr f32 kStill = 0.01f;     ///< a slide slower than this is over
    static constexpr f32 kShoveLimit = 0.5f; ///< of the character's pace, a shove's most

    /** A hit's push (the way it travels, as long as its maker gave it) and its flags. */
    void queue(const Vec3& push, u32 flags, f32 damage);
    /** Turns what the last frame's hits pushed into a kick, the heaviest flag's; the heading
     * the body now faces, or nullopt when nothing kicked. */
    std::optional<f32> kick(f32 facing, bool pojo);
    /** Where the slide carries the body over `seconds` at a character's `pace`, the slide
     * decaying after. */
    Vec3 step(f32 seconds, f32 pace);
    /** Another member's step of `seconds` ran into the character: it is shoved by it, as
     * by the step a 30 Hz frame would have made. */
    void shove(const Vec3& displacement, f32 seconds);
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
    bool m_fast = false; ///< the frame after a fall's kick travels freely
    Vec3 m_shove{0.0f};  ///< units a second
    bool m_shoved = false;
    bool m_pushed = false;
};

} // namespace gdl::game
