#include "expansions/n64/level/LegendsBossEruption.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Error.h"
#include "engine/math/Mt19937.h"

namespace gdl::game {

LegendsBossEruption::LegendsBossEruption(std::span<const formats::LegendsMapObject> objects) {
    // A private deterministic visual stream must not consume gameplay randomness
    // or use a wall clock. Native random ranges are retained, not the N64 RNG seed.
    Mt19937 random{0x534b4f31U};
    for (usize index = 0; index < objects.size(); ++index) {
        const auto& object = objects[index];
        if ((object.flags & 0x200U) == 0 || !object.name.starts_with("ROCKFLY")) {
            continue;
        }
        if (m_fragments.size() == kMaximumFragments) {
            throw FormatError("N64 boss entrance: excessive fragment roster");
        }
        const Vec3 anchor{object.transform[3]};
        if (glm::any(glm::isnan(anchor)) || glm::any(glm::isinf(anchor)) ||
            glm::any(glm::greaterThan(glm::abs(anchor), Vec3{100000}))) {
            throw FormatError("N64 boss entrance: invalid fragment anchor");
        }
        Fragment fragment;
        fragment.object = index;
        fragment.anchor = anchor;
        // e00763ac uses three floor(random15*9/32768) spin bytes, then
        // vertical velocity random15*20/32768. e00784d8 advances position
        // before gravity (byte0x20 doubled), and rotates at byte*30 degrees/s.
        const auto spin = [&] { return static_cast<f32>(((random() >> 17) * 9U) >> 15); };
        const Vec3 angles{degreesToRadians(spin()), degreesToRadians(spin()),
                          -degreesToRadians(spin())};
        const Mat4 turn = glm::rotate(
            glm::rotate(glm::rotate(Mat4{1}, angles.x, Vec3{1, 0, 0}), angles.y, Vec3{0, 1, 0}),
            angles.z, Vec3{0, 0, 1});
        f32 velocity = static_cast<f32>(random() >> 17) * (20.0f / 32768.0f);
        Mat4 rotation{1};
        for (usize tick = 1; tick < fragment.poses.size(); ++tick) {
            auto& pose = fragment.poses[tick];
            pose.height = fragment.poses[tick - 1].height + velocity / 30.0f;
            velocity -= 64.0f / 30.0f;
            rotation *= turn;
            pose.rotation = glm::normalize(glm::quat_cast(rotation));
        }
        m_fragments.push_back(std::move(fragment));
    }
}

bool LegendsBossEruption::begin() {
    if (m_started || m_fragments.empty()) {
        return false;
    }
    m_started = true;
    m_age = m_previousAge = 0;
    return true;
}

void LegendsBossEruption::capturePresentation() {
    m_previousAge = m_age;
}

void LegendsBossEruption::update(f32 seconds) {
    if (m_started && std::isfinite(seconds) && seconds > 0) {
        m_age = std::min(kLifetime, m_age + static_cast<f64>(seconds));
    }
}

const LegendsBossEruption::Fragment* LegendsBossEruption::find(usize object) const {
    const auto found = std::ranges::find(m_fragments, object, &Fragment::object);
    return found == m_fragments.end() ? nullptr : &*found;
}

std::optional<bool> LegendsBossEruption::visible(usize object) const {
    // These FLOOR_01/FLRDESTSIDE/FLRDESTBOTTOM pieces form the arena's intact cap.
    // N64 e003955c initially hides them, but its script reveal is not recovered.
    // Match GDL's SkorneArena/FallingPiece entrance contract: show the authored
    // cap at rest, release it on the encounter cue, then retire the spent pieces.
    return find(object) ? std::optional{!m_started || m_age < kLifetime} : std::nullopt;
}

LegendsBossEruption::Pose LegendsBossEruption::sample(const Fragment& fragment, f32 alpha) const {
    const f64 age =
        alpha < 0 || !std::isfinite(alpha)
            ? m_age
            : std::lerp(m_previousAge, m_age, static_cast<f64>(std::clamp(alpha, 0.0f, 1.0f)));
    const f64 tick = std::clamp(age * 30, 0.0, 75.0);
    const auto first = static_cast<usize>(tick);
    const auto next = std::min(first + 1, fragment.poses.size() - 1);
    const f32 blend = static_cast<f32>(tick - static_cast<f64>(first));
    return {std::lerp(fragment.poses[first].height, fragment.poses[next].height, blend),
            glm::slerp(fragment.poses[first].rotation, fragment.poses[next].rotation, blend)};
}

Mat4 LegendsBossEruption::transform(usize object, f32 alpha) const {
    const auto* fragment = find(object);
    if (!fragment || !m_started) {
        return Mat4{1};
    }
    const auto pose = sample(*fragment, alpha);
    return glm::translate(Mat4{1}, fragment->anchor + Vec3{0, pose.height, 0}) *
           glm::mat4_cast(pose.rotation) * glm::translate(Mat4{1}, -fragment->anchor);
}

Vec3 LegendsBossEruption::offset(usize object, f32 alpha) const {
    const auto* fragment = find(object);
    return fragment && m_started ? Vec3{0, sample(*fragment, alpha).height, 0} : Vec3{0};
}

} // namespace gdl::game
