#pragma once

#include <array>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"

namespace gdl {

/** A value over a particle's time: from its birth to its life's end, then over its fade to
 * its death. */
struct ParticleEnvelope {
    f32 lifeStart = 0.0f;
    f32 lifeEnd = 0.0f;
    f32 fadeStart = 0.0f;
    f32 fadeEnd = 0.0f;

    /** The value at `age` frames of a particle living `life` frames and fading `fade` more. */
    f32 at(f32 age, f32 life, f32 fade) const;
};

/**
 * How a particle system emits and draws, in game frames and world units: a template laid
 * over the built-in preset it starts from, the way the original resolves one. Times are
 * frames, rates particles a frame, speeds units a frame and colours 0 to 255.
 */
struct ParticleDescriptor {
    static constexpr f32 kFrameRate = 30.0f;
    static constexpr f32 kSphere = -1.0f;   ///< the angle of directions spread every way
    static constexpr u32 kEndless = 0xFFFF; ///< a phase that never ends
    static constexpr u32 kMostParticles = 300;
    static constexpr f32 kDefaultGravity = -32.0f / 900.0f;
    static constexpr f32 kLeastWidth = 1.0f / 450.0f;

    u32 delay = 0;
    u32 emitFrames = 1; ///< the emitting phase, kEndless to stay in it
    u32 fadeFrames = 0; ///< the fading phase after it, kEndless to stay in it
    bool forever = false; ///< emitting starts over after fading
    bool oneShot = false; ///< every particle at once
    bool dynamic = false; ///< positions and directions follow the marker each frame
    f32 angle = 0.0f;     ///< the cone's half angle in radians; 0 for the direction alone
    Vec3 direction{0.0f, 1.0f, 0.0f};
    Vec3 volume{0.0f, 0.0f, 0.0f};
    std::array<f32, 4> rate{0.0f, 0.0f, 0.0f, 0.0f}; ///< at the start and end of each phase
    f32 rateRandom = 0.0f;
    f32 gravity = 0.0f; ///< added to the height each frame squared; up when positive
    f32 drag = 0.0f;
    f32 speed = 0.0f;
    u32 maxParticles = 0; ///< 0 leaves it to the rates and lives
    u32 maxDirections = 0;
    u32 maxPositions = 0;
    u32 particleLife = 1;
    u32 particleFade = 0;
    ParticleEnvelope red;
    ParticleEnvelope green;
    ParticleEnvelope blue;
    ParticleEnvelope alpha;
    ParticleEnvelope width;
    std::string texture;
    bool additive = false;
    bool multiply = false;
    bool sorted = false;
    bool depthTest = true;
    bool depthWrite = true;

    /** The descriptor a template resolves to: its preset's fields, then its own. */
    static ParticleDescriptor fromTemplate(const ParticleTemplate& source);
    /** Lays a template's filled fields and decided flags over this. */
    void apply(const ParticleTemplate& source);
    /** How many particles can live at once. */
    u32 capacity() const;
};

/** The console's built-in presets, found by their `preset` number. */
std::span<const ParticleTemplate> particlePresets();

/** One live particle: where it started, how it moves and how old it is, in frames. */
struct Particle {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    f32 age = 0.0f;
};

/**
 * One emitter run the way the original runs it: after its delay it emits at its phase's rate
 * for its emitting frames, then at the fading rate, then stops (or starts over); each
 * particle leaves a random point of the volume along a direction inside the cone and flies
 * straight, bent by gravity, its colour and width following their envelopes until it dies.
 */
class ParticleEmitter {
public:
    enum class Phase : u8 { Delay, Emitting, Fading, Done };
    static constexpr u32 kMostFramesAtOnce = 15; ///< a longer gap counts as one frame

    void start(const ParticleDescriptor& descriptor, const Mat4& node, u32 seed = 1);
    /** Moves the marker; new particles leave from there. */
    void setNode(const Mat4& node) { m_node = node; }
    const Mat4& node() const { return m_node; }
    /** Ends the emission; the particles live out their time. */
    void finish() { m_phase = Phase::Done; }
    /** Ages the particles `frames` on, dropping the dead, then emits the frame's share. */
    void step(u32 frames);

    bool active() const { return m_phase != Phase::Done || !m_particles.empty(); }
    Phase phase() const { return m_phase; }
    u32 age() const { return m_age; }
    const ParticleDescriptor& descriptor() const { return m_descriptor; }
    std::span<const Particle> particles() const { return m_particles; }
    Vec3 positionOf(const Particle& particle) const;
    Color colorOf(const Particle& particle) const;
    f32 widthOf(const Particle& particle) const;

    /** Appends a camera-facing square per particle, `right` and `up` being the camera's. */
    void draw(ImmediateBatch& batch, const Vec3& right, const Vec3& up) const;

private:
    f32 rateNow();
    void emit(f32 age);
    Vec3 newOrigin();
    Vec3 newVelocity();
    f32 random01();

    ParticleDescriptor m_descriptor;
    Mat4 m_node{1.0f};
    std::vector<Particle> m_particles;
    Phase m_phase = Phase::Done;
    u32 m_age = 0;
    f32 m_saved = 0.0f; ///< the fraction of a particle owed from the last frame
    std::minstd_rand m_random;
};

} // namespace gdl
