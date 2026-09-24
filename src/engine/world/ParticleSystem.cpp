#include "engine/world/ParticleSystem.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Strings.h"
#include "engine/core/Types.h"

namespace gdl {

namespace {

constexpr f32 kColorScale = 255.0f; ///< colour lanes keep their byte values
constexpr f32 kMostWidth = 100000.0f;

f32 lerp(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

f32 clampFrames(f32 seconds, f32 least, f32 most) {
    return std::clamp(seconds * ParticleDescriptor::kFrameRate, least, most);
}

/** One lane of the packed colours as an envelope. */
ParticleEnvelope laneOf(const std::array<u32, 4>& rgba, u32 shift) {
    const auto lane = [&](usize i) {
        return std::clamp(static_cast<f32>((rgba[i] >> shift) & 0xFFU), 0.0f, kColorScale);
    };
    return ParticleEnvelope{lane(0), lane(1), lane(2), lane(3)};
}

/** A template of the console's table: `preset` numbers it; only its filled fields count. */
ParticleTemplate preset(u32 number, u32 flags, u32 enables, std::array<f32, 2> emitterLife,
                        std::array<f32, 2> particleLife, f32 angle, std::array<f32, 4> rate,
                        f32 rateRandom, f32 gravity, f32 speed, std::array<u32, 4> rgba,
                        std::array<f32, 4> width) {
    ParticleTemplate t;
    t.preset = number;
    t.flags = flags;
    t.flagMask = flags;
    t.enables = enables;
    t.emitterLife = emitterLife;
    t.particleLife = particleLife;
    t.angle = angle;
    t.rate = rate;
    t.rateRandom = rateRandom;
    t.gravity = gravity;
    t.speed = speed;
    t.rgba = rgba;
    t.width = width;
    return t;
}

const std::array<ParticleTemplate, 8> kPresets{
    preset(0, 0x800, 0x72270, {999.0f, 999.0f}, {0.4f, 0.4f}, 360.0f,
           {100.0f, 100.0f, 100.0f, 100.0f}, 0.0f, 0.0f, 10.0f,
           {0xFFFF0000, 0xFF0000FF, 0xFF0000FF, 0x000000FF}, {0.1f, 1.0f, 1.0f, 0.1f}),
    preset(1, 0x800, 0x72670, {999.0f, 999.0f}, {10.0f, 5.0f}, 0.0f, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f,
           0.0f, 15.0f, {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}, {1.0f, 1.0f, 1.0f, 1.0f}),
    preset(2, 0x800, 0x72270, {999.0f, 999.0f}, {0.4f, 0.3f}, 20.0f, {60.0f, 60.0f, 60.0f, 60.0f},
           0.0f, 0.0f, 15.0f, {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00FFFFFF},
           {0.3f, 0.3f, 0.3f, 0.3f}),
    preset(3, 0x800, 0x72270, {999.0f, 999.0f}, {1.0f, 5.0f}, 40.0f, {7.0f, 7.0f, 7.0f, 7.0f}, 0.0f,
           0.0f, 3.0f, {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00FFFFFF}, {1.0f, 3.0f, 3.0f, 7.0f}),
    preset(4, 0x808, 0x72270, {999.0f, 999.0f}, {0.4f, 0.4f}, 360.0f,
           {100.0f, 100.0f, 100.0f, 100.0f}, 0.0f, 0.0f, 10.0f,
           {0xFFFF0000, 0xFF0000FF, 0xFF0000FF, 0x000000FF}, {0.1f, 1.0f, 1.0f, 0.1f}),
    preset(5, 0x808, 0x72670, {999.0f, 999.0f}, {10.0f, 5.0f}, 0.0f, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f,
           0.0f, 45.0f, {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF}, {1.0f, 1.0f, 1.0f, 1.0f}),
    preset(6, 0x808, 0x72270, {999.0f, 999.0f}, {1.0f, 0.0f}, 20.0f, {60.0f, 60.0f, 60.0f, 60.0f},
           0.0f, 0.0f, 30.0f, {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
           {0.3f, 0.3f, 0.3f, 0.3f}),
    preset(7, 0x808, 0x72A70, {999.0f, 999.0f}, {1.0f, 5.0f}, 360.0f, {7.0f, 7.0f, 7.0f, 7.0f},
           0.0f, -0.1f, 3.0f, {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
           {1.0f, 3.0f, 3.0f, 7.0f}),
};

/** Two unit vectors spanning the plane across `direction`. */
void orthogonalPair(const Vec3& direction, Vec3& a, Vec3& b) {
    const Vec3 axis =
        std::abs(direction.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    a = glm::normalize(glm::cross(direction, axis));
    b = glm::normalize(glm::cross(direction, a));
}

} // namespace

f32 ParticleEnvelope::at(f32 age, f32 life, f32 fade) const {
    if (age < life) {
        return lerp(lifeStart, lifeEnd, life > 0.0f ? age / life : 1.0f);
    }
    if (fade <= 0.0f) {
        return fadeEnd;
    }
    return lerp(fadeStart, fadeEnd, std::min((age - life) / fade, 1.0f));
}

std::span<const ParticleTemplate> particlePresets() {
    return kPresets;
}

ParticleDescriptor ParticleDescriptor::fromTemplate(const ParticleTemplate& source) {
    ParticleDescriptor out;
    if (source.sets(ParticleTemplate::kPreset)) {
        for (const ParticleTemplate& candidate : kPresets) {
            if (candidate.preset == source.preset) {
                out.apply(candidate);
                break;
            }
        }
    }
    out.apply(source);
    return out;
}

void ParticleDescriptor::apply(const ParticleTemplate& source) {
    using T = ParticleTemplate;
    if (source.sets(T::kMaxParticles)) {
        maxParticles = static_cast<u32>(std::max(source.maxParticles, 0));
    }
    if (source.sets(T::kMaxDirections)) {
        maxDirections = source.maxDirections;
    }
    if (source.sets(T::kMaxPositions)) {
        maxPositions = source.maxPositions;
    }
    if (source.sets(T::kEmitterLife)) {
        emitFrames = static_cast<u32>(clampFrames(source.emitterLife[0], 1.0f, 65535.0f));
        fadeFrames = static_cast<u32>(clampFrames(source.emitterLife[1], 0.0f, 65535.0f));
        if (source.emitterLife[0] < 0.0f) {
            emitFrames = kEndless;
            forever = true;
        } else if (source.emitterLife[1] < 0.0f) {
            fadeFrames = kEndless;
            forever = true;
        }
    }
    if (source.sets(T::kParticleLife)) {
        particleLife = static_cast<u32>(clampFrames(source.particleLife[0], 1.0f, 255.0f));
        particleFade = static_cast<u32>(clampFrames(source.particleLife[1], 0.0f, 255.0f));
    }
    if (source.sets(T::kAngle)) {
        const f32 degrees = source.angle;
        if (degrees < 0.0f || degrees >= 359.0f) {
            angle = kSphere;
        } else if (degrees < 1.0f) {
            angle = 0.0f;
        } else {
            angle = kPi * degrees / 360.0f;
        }
    }
    if (source.sets(T::kDelay)) {
        delay = static_cast<u32>(std::max(source.delay * kFrameRate, 0.0f));
    }
    if (source.sets(T::kDirection)) {
        direction = source.direction;
    }
    if (source.sets(T::kVolume)) {
        volume = source.volume;
    }
    if (source.sets(T::kRate)) {
        for (usize i = 0; i < rate.size(); ++i) {
            rate[i] = source.rate[i] / kFrameRate;
        }
    }
    if (source.sets(T::kRateRandom)) {
        rateRandom = 0.01f * source.rateRandom;
    }
    if (source.sets(T::kGravity)) {
        gravity = kDefaultGravity * source.gravity;
    }
    if (source.sets(T::kDrag)) {
        drag = -source.drag;
    }
    if (source.sets(T::kSpeed)) {
        speed = source.speed / kFrameRate;
    }
    if (source.sets(T::kColor)) {
        red = laneOf(source.rgba, 16U);
        green = laneOf(source.rgba, 8U);
        blue = laneOf(source.rgba, 0U);
    }
    if (source.sets(T::kAlpha)) {
        alpha = laneOf(source.rgba, 24U);
    }
    if (source.sets(T::kWidth)) {
        const auto lane = [&](usize i) {
            return std::clamp(source.width[i], kLeastWidth, kMostWidth);
        };
        width = ParticleEnvelope{lane(0), lane(1), lane(2), lane(3)};
    }
    if (source.sets(T::kTexture)) {
        texture = normalizeAssetName(source.texture);
    }
    if (source.flag(T::kDefaultGravity) && gravity == 0.0f) {
        gravity = kDefaultGravity;
    }
    if (source.flag(T::kDefaultDrag) && drag == 0.0f) {
        drag = -1.0f;
    }
    if (source.decides(T::kDynamic)) {
        dynamic = source.flag(T::kDynamic);
    }
    if (source.decides(T::kOneShot)) {
        oneShot = source.flag(T::kOneShot);
    }
    if (source.decides(T::kForever)) {
        forever = source.flag(T::kForever);
    }
    if (source.decides(T::kMultiply)) {
        multiply = source.flag(T::kMultiply);
    }
    if (source.decides(T::kAdditive)) {
        additive = source.flag(T::kAdditive);
    }
    if (source.decides(T::kSorted)) {
        sorted = source.flag(T::kSorted);
    }
    if (source.decides(T::kNoDepthTest)) {
        depthTest = !source.flag(T::kNoDepthTest);
    }
    if (source.decides(T::kNoDepthWrite)) {
        depthWrite = !source.flag(T::kNoDepthWrite);
    }
}

u32 ParticleDescriptor::capacity() const {
    if (oneShot || maxParticles != 0) {
        const u32 count = maxParticles != 0 ? maxParticles : static_cast<u32>(rate[0] * kFrameRate);
        return std::max(count, 1U);
    }
    // Enough for the fastest rate to fill a whole life, as the original estimates it.
    const f32 fastest = *std::max_element(rate.begin(), rate.end());
    const auto life = static_cast<f32>(particleLife + particleFade);
    const auto needed = static_cast<u32>(std::ceil(fastest * life));
    return std::clamp(needed, 1U, kMostParticles);
}

void ParticleEmitter::start(const ParticleDescriptor& descriptor, const Mat4& node, u32 seed) {
    m_descriptor = descriptor;
    m_node = node;
    m_particles.clear();
    m_particles.reserve(descriptor.capacity());
    m_phase = descriptor.delay > 0 ? Phase::Delay : Phase::Emitting;
    m_age = 0;
    m_saved = 0.0f;
    m_random.seed(seed);
}

f32 ParticleEmitter::random01() {
    return static_cast<f32>(m_random() - std::minstd_rand::min()) /
           static_cast<f32>(std::minstd_rand::max() - std::minstd_rand::min());
}

/** The emission rate this frame, moving the phases on as the age demands. */
f32 ParticleEmitter::rateNow() {
    const ParticleDescriptor& d = m_descriptor;
    if (m_phase == Phase::Emitting) {
        if (d.oneShot) {
            m_phase = Phase::Done;
            return static_cast<f32>(d.capacity());
        }
        if (d.emitFrames == ParticleDescriptor::kEndless) {
            return d.rate[0];
        }
        if (m_age <= d.emitFrames) {
            return lerp(d.rate[0], d.rate[1],
                        static_cast<f32>(m_age) / static_cast<f32>(d.emitFrames));
        }
        m_phase = Phase::Fading;
    }
    if (m_phase == Phase::Fading) {
        if (d.fadeFrames == ParticleDescriptor::kEndless) {
            return d.rate[2];
        }
        if (m_age <= d.emitFrames + d.fadeFrames) {
            const auto into = static_cast<f32>(m_age - d.emitFrames);
            return lerp(d.rate[2], d.rate[3],
                        d.fadeFrames > 0 ? into / static_cast<f32>(d.fadeFrames) : 1.0f);
        }
        if (d.forever) {
            m_phase = Phase::Emitting;
            m_age = 0;
            return d.rate[0];
        }
        m_phase = Phase::Done;
    }
    return 0.0f;
}

void ParticleEmitter::step(u32 frames) {
    if (frames == 0) {
        return;
    }
    const u32 dt = frames > kMostFramesAtOnce ? 1U : frames;
    const ParticleDescriptor& d = m_descriptor;
    const auto span = static_cast<f32>(d.particleLife + d.particleFade);
    for (Particle& particle : m_particles) {
        particle.age += static_cast<f32>(dt);
    }
    std::erase_if(m_particles, [&](const Particle& p) { return p.age >= span; });

    u32 elapsed = dt;
    if (m_phase == Phase::Delay) {
        m_age += dt;
        if (m_age <= d.delay) {
            return;
        }
        elapsed = m_age - d.delay;
        m_age = elapsed;
        m_phase = Phase::Emitting;
    } else if (m_phase != Phase::Done) {
        m_age += dt;
    }
    f32 rate = rateNow();
    if (rate > 0.0f && d.rateRandom > 0.0f) {
        rate *= 1.0f + d.rateRandom * (2.0f * random01() - 1.0f);
    }
    // Whole particles come out of the frame's share plus what the last one owed; a partial
    // one is owed again.
    f32 budget = rate * static_cast<f32>(elapsed) + m_saved;
    const u32 capacity = d.capacity();
    u32 count = 0;
    while (budget > 0.0f && m_particles.size() < capacity) {
        const f32 age = rate > 0.0f
                            ? std::min(static_cast<f32>(count) / rate, static_cast<f32>(elapsed))
                            : 0.0f;
        emit(age);
        budget -= 1.0f;
        ++count;
    }
    m_saved = budget > 0.0f ? 0.0f : budget;
}

void ParticleEmitter::emit(f32 age) {
    Particle particle;
    particle.origin = newOrigin();
    particle.velocity = newVelocity();
    particle.age = age;
    m_particles.push_back(particle);
}

/** A random point of the volume about the marker, or the marker itself without one. */
Vec3 ParticleEmitter::newOrigin() {
    const Vec3& volume = m_descriptor.volume;
    if (volume.x == 0.0f && volume.y == 0.0f && volume.z == 0.0f) {
        return Vec3{m_node[3]};
    }
    const Vec3 local{volume.x * (random01() - 0.5f), volume.y * (random01() - 0.5f),
                     volume.z * (random01() - 0.5f)};
    return Vec3{m_node * Vec4{local, 1.0f}};
}

/** The direction through the marker's frame at the speed, spread into the cone. */
Vec3 ParticleEmitter::newVelocity() {
    const ParticleDescriptor& d = m_descriptor;
    // Stationary particles can still rise under gravity. There is no launch direction
    // to spread when speed is zero; normalizing it would make every coordinate NaN.
    if (d.speed == 0.0f) {
        return Vec3{0.0f};
    }
    if (d.angle == ParticleDescriptor::kSphere) {
        const Vec3 random{random01() - 0.5f, random01() - 0.5f, random01() - 0.5f};
        const f32 length = glm::length(random);
        return (length > 0.0f ? random / length : Vec3{0.0f, 1.0f, 0.0f}) * d.speed;
    }
    Vec3 forward = Mat3{m_node} * d.direction;
    const f32 squared = glm::dot(forward, forward);
    if (squared < 0.7f || squared > 1.3f) {
        forward = squared > 0.0f ? forward / std::sqrt(squared) : Vec3{0.0f, 1.0f, 0.0f};
    }
    forward *= d.speed;
    if (d.angle == 0.0f) {
        return forward;
    }
    Vec3 a;
    Vec3 b;
    orthogonalPair(glm::normalize(forward), a, b);
    const f32 tilt = d.angle * random01();
    const f32 turn = kPi * random01();
    const f32 side = (m_random() & 4U) != 0 ? -std::cos(turn) : std::cos(turn);
    return forward * std::cos(tilt) + (a * std::sin(turn) + b * side) * (std::sin(tilt) * d.speed);
}

Vec3 ParticleEmitter::positionOf(const Particle& particle) const {
    const f32 t = particle.age;
    Vec3 position = particle.origin + particle.velocity * t;
    position.y += m_descriptor.gravity * t * t;
    return position;
}

Color ParticleEmitter::colorOf(const Particle& particle) const {
    const ParticleDescriptor& d = m_descriptor;
    const auto life = static_cast<f32>(d.particleLife);
    const auto fade = static_cast<f32>(d.particleFade);
    const auto lane = [&](const ParticleEnvelope& envelope) {
        return static_cast<u8>(std::clamp(envelope.at(particle.age, life, fade), 0.0f, 255.0f));
    };
    return Color::rgba(lane(d.red), lane(d.green), lane(d.blue), lane(d.alpha));
}

f32 ParticleEmitter::widthOf(const Particle& particle) const {
    const ParticleDescriptor& d = m_descriptor;
    return d.width.at(particle.age, static_cast<f32>(d.particleLife),
                      static_cast<f32>(d.particleFade));
}

void ParticleEmitter::draw(ImmediateBatch& batch, const Vec3& right, const Vec3& up) const {
    for (const Particle& particle : m_particles) {
        const Vec3 centre = positionOf(particle);
        const Color color = colorOf(particle);
        const f32 half = 0.5f * widthOf(particle);
        const Vec3 dx = right * half;
        const Vec3 dy = up * half;
        const Vec3 topLeft = centre - dx + dy;
        const Vec3 topRight = centre + dx + dy;
        const Vec3 bottomLeft = centre - dx - dy;
        const Vec3 bottomRight = centre + dx - dy;
        batch.vertex(topLeft, color, Vec2{0.0f, 0.0f});
        batch.vertex(topRight, color, Vec2{1.0f, 0.0f});
        batch.vertex(bottomRight, color, Vec2{1.0f, 1.0f});
        batch.vertex(topLeft, color, Vec2{0.0f, 0.0f});
        batch.vertex(bottomRight, color, Vec2{1.0f, 1.0f});
        batch.vertex(bottomLeft, color, Vec2{0.0f, 1.0f});
    }
}

} // namespace gdl
