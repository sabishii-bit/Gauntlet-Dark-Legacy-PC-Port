#pragma once

#include <array>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"

namespace gdl::game {

/** Native type9 trap presentation (e0068118), independent of ownership/grants.
 * Its two authored models are static; the transition is a primitive-colour
 * blend and a separate, rising particle emitter. */
class LegendsObeliskMotion {
public:
    static constexpr usize kCapacity = 100;
    struct Particle {
        std::array<s32, 3> coordinate{}; ///< native fixed-point world units, 1/64
        std::array<s8, 3> direction{};
        u16 speed = 0; ///< native fixed-point units/second, 1/1024
        u16 age = 0;   ///< native 150 Hz ticks
        u8 gravity = 1;
        u8 alpha = 0;
        bool alive = false;
        Vec3 position() const;
        bool operator==(const Particle&) const = default;
    };

    /** False after the first activation, including quiet restoration. The
     * cosmetic seed uses the native LCG, not the unrelated gameplay RNG. */
    bool activate(const Vec3& origin, u32 seed);
    void restoreSpent();
    void advance(f32 seconds);
    bool started() const { return m_started; }
    bool spentModel() const { return m_spent; }
    f32 whiteBlend() const { return static_cast<f32>(m_white) / 255; }
    f32 spriteAngle() const { return static_cast<f32>(m_ticks) * (20.0f / 30); }
    Vec3 emitterPosition() const;
    std::span<const Particle> particles() const { return m_particles; }
    usize liveParticles() const;

private:
    void tick();
    bool emit(const Vec3& position);
    u32 random15();
    std::array<Particle, kCapacity> m_particles{};
    Vec3 m_origin{0};
    Vec3 m_previousEmitter{0};
    f64 m_remainder = 0;
    u32 m_random = 0;
    u32 m_ticks = 0;
    s32 m_emissionCountdown = 0;
    u8 m_transition = 0;
    u8 m_white = 0;
    bool m_started = false;
    bool m_spent = false;
};

/** e002180c's FC3099FF/5F0EFE3F: (white-green)*texel+green.
 * This private image preserves native texture alpha and leaves its lender intact. */
Image legendsObeliskSparkle(const Image& source);

} // namespace gdl::game
