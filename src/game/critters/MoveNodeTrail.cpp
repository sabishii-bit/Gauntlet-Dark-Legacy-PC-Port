#include "game/critters/MoveNodeTrail.h"

#include <optional>

#include "engine/math/ApproximateLength.h"

namespace gdl::game {

void MoveNodeTrail::step(f32 seconds, const Vec3& world, const Vec3& local, const Mat4& model,
                         bool part) {
    const Sample now{world, local, 0.0f};
    if (seconds <= 0.0f) {
        if (m_count == 0) {
            ++m_count;
        }
        m_samples[m_count - 1] = now;
        return;
    }
    for (usize i = 0; i < m_count; ++i) {
        m_samples[i].age += seconds;
    }
    std::optional<usize> reference;
    for (usize i = m_count; i-- > 0;) {
        if (m_samples[i].age >= kFrameSeconds - kFrameSlack) {
            reference = i;
            break;
        }
    }
    // A clock faster than the samples span measures from the oldest it holds.
    if (!reference.has_value() && m_count == kSamples) {
        reference = 0;
    }
    if (!reference.has_value()) {
        m_travel = Vec3{0.0f};
    } else if (part) {
        m_travel = Vec3{model * Vec4{local - m_samples[*reference].local, 0.0f}};
    } else {
        m_travel = world - m_samples[*reference].world;
    }
    // What is older than the reference is never measured from again; the reference may be,
    // when the next update is a short one.
    usize first = reference.value_or(0);
    if (m_count - first == kSamples) {
        ++first;
    }
    for (usize i = first; i < m_count; ++i) {
        m_samples[i - first] = m_samples[i];
    }
    for (usize i = m_count - first; i < m_count; ++i) {
        m_samples[i] = Sample{};
    }
    m_count -= first;
    m_samples[m_count++] = now;
}

Vec3 MoveNodeTrail::blowPush(const Vec3& body, const Vec3& player) const {
    const Vec3 away{player.x - body.x, kLift, player.z - body.z};
    return kPushScale * (approximateUnit(m_travel) + approximateUnit(away));
}

} // namespace gdl::game
