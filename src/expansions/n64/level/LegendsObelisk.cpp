#include "expansions/n64/level/LegendsObelisk.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gdl::game {

Vec3 LegendsObeliskMotion::Particle::position() const {
    return Vec3{coordinate[0], coordinate[1], coordinate[2]} / 64.0f;
}

bool LegendsObeliskMotion::activate(const Vec3& origin, u32 seed) {
    if (m_started || !std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        !std::isfinite(origin.z) || glm::any(glm::greaterThan(glm::abs(origin), Vec3{1000000}))) {
        return false;
    }
    m_started = true;
    m_origin = origin;
    m_previousEmitter = emitterPosition();
    m_random = seed;
    return true;
}

void LegendsObeliskMotion::restoreSpent() {
    m_started = true;
    m_spent = true;
    m_transition = 11;
    m_ticks = 111;
    m_white = 0;
    m_remainder = 0;
    m_particles = {};
}

usize LegendsObeliskMotion::liveParticles() const {
    return static_cast<usize>(std::ranges::count(m_particles, true, &Particle::alive));
}

Vec3 LegendsObeliskMotion::emitterPosition() const {
    // e00761b8 applies 30 degrees per native frame to the parent. Its child
    // remains three units along local +z, while the parent rises 3 units/s.
    const f32 angle = static_cast<f32>(m_ticks) * (std::numbers::pi_v<f32> / 6);
    return m_origin +
           Vec3{3 * std::sin(angle), static_cast<f32>(m_ticks) / 10, 3 * std::cos(angle)};
}

u32 LegendsObeliskMotion::random15() {
    m_random = m_random * 0x41c64e6dU + 0x3039U; // e0010c90
    return (m_random >> 16) & 0x7fff;
}

bool LegendsObeliskMotion::emit(const Vec3& position) {
    auto free = std::ranges::find(m_particles, false, &Particle::alive);
    if (free == m_particles.end()) {
        return false;
    }
    auto& particle = *free;
    particle = {};
    for (usize axis = 0; axis < 3; ++axis) {
        particle.coordinate[axis] = static_cast<s32>(position[static_cast<s32>(axis)] * 64);
    }
    // e0023188/e0023b20: full yaw and pitch, speed [3,4), initial age [0,45).
    constexpr f32 kAngleScale = 2 * std::numbers::pi_v<f32> / 32768;
    const f32 yaw = static_cast<f32>(random15()) * kAngleScale - std::numbers::pi_v<f32>;
    const f32 pitch = static_cast<f32>(random15()) * kAngleScale - std::numbers::pi_v<f32>;
    particle.direction = {static_cast<s8>(-std::sin(yaw) * std::cos(pitch) * 127),
                          static_cast<s8>(std::sin(pitch) * 127),
                          static_cast<s8>(std::cos(yaw) * std::cos(pitch) * 127)};
    particle.speed = static_cast<u16>(3072 + (random15() >> 5));
    particle.age = static_cast<u16>((random15() * 45) >> 15);
    particle.alpha = 255;
    particle.alive = true;
    return true;
}

void LegendsObeliskMotion::tick() {
    if (m_transition < 11) {
        const u32 counter = m_transition++;
        m_white = static_cast<u8>(counter < 6 ? counter * 51 : 255 - (counter - 5) * 51);
        if (m_transition == 6) {
            m_spent = true;
        } else if (m_transition == 11) {
            m_white = 0;
        }
    }
    ++m_ticks;
    const Vec3 emitter = emitterPosition();
    // e00759ac's emitter lasts 450 ticks; its 105-tick particles remain alive
    // after emission stops. e0023f34's initial countdown emits seven, then five
    // per native frame, interpolating the moving emitter for each birth.
    if (m_ticks <= 90) {
        const s32 previousCountdown = m_emissionCountdown;
        f32 fraction = static_cast<f32>(previousCountdown) / 5;
        m_emissionCountdown -= 5;
        while (m_emissionCountdown <= 1) {
            if (!emit(glm::mix(m_previousEmitter, emitter, fraction))) {
                m_emissionCountdown = 0;
                break;
            }
            ++m_emissionCountdown;
            fraction += 1.0f / static_cast<f32>(5 + previousCountdown);
        }
    }
    m_previousEmitter = emitter;
    for (auto& particle : m_particles) {
        if (!particle.alive) {
            continue;
        }
        particle.age += 5;
        const s32 fade = particle.age > 22 ? (particle.age - 22) * 256 / 83 : 0;
        if (particle.age > 105 || fade >= 255) {
            particle.alive = false;
            continue;
        }
        particle.alpha = static_cast<u8>(255 - fade);
        const s32 travel = particle.speed * 5 / 150;
        for (usize axis = 0; axis < 3; ++axis) {
            particle.coordinate[axis] += (particle.direction[axis] * travel) >> 11;
        }
        particle.speed = static_cast<u16>(std::max(0, static_cast<s32>(particle.speed) - 160));
        particle.coordinate[1] -= particle.gravity;
        particle.gravity = static_cast<u8>(std::min(255, particle.gravity + 1));
    }
}

void LegendsObeliskMotion::advance(f32 seconds) {
    if (!m_started || !(seconds > 0) || !std::isfinite(seconds) || m_ticks >= 111) {
        return;
    }
    m_remainder += static_cast<f64>(seconds);
    constexpr f64 kStep = 1.0 / 30;
    while (m_remainder + 1.0e-8 >= kStep && m_ticks < 111) {
        m_remainder -= kStep;
        tick();
    }
}

Image legendsObeliskSparkle(const Image& source) {
    Image result = source;
    for (usize offset = 0; offset + 3 < result.pixels.size(); offset += 4) {
        result.pixels[offset] =
            static_cast<u8>(static_cast<u32>(result.pixels[offset]) * 248 / 255);
        result.pixels[offset + 1] = 248;
        result.pixels[offset + 2] =
            static_cast<u8>(static_cast<u32>(result.pixels[offset + 2]) * 248 / 255);
    }
    return result;
}

} // namespace gdl::game
