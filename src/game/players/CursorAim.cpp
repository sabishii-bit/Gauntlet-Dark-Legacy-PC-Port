#include "game/players/CursorAim.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {
std::optional<Vec3> cursorAimDirection(Vec2 cursor, const Mat4& clip, const Vec3& position) {
    constexpr f32 kCursorDeadZone = 0.004f; // NDC: two thousandths of the window extent
    constexpr f32 kProjectionEpsilon = 1.0e-6f;
    if (!std::isfinite(cursor.x) || !std::isfinite(cursor.y) || cursor.x < 0 || cursor.x > 1 ||
        cursor.y < 0 || cursor.y > 1) {
        return {};
    }
    const Vec4 projected = clip * Vec4{position, 1};
    if (!std::isfinite(projected.w) || projected.w <= kProjectionEpsilon) {
        return {};
    }
    const Vec2 screen = Vec2{projected} / projected.w;
    const Vec2 delta = cursor * 2.0f - Vec2{1} - screen;
    // Invert the projected horizontal axes at the player, not a ray's world hit.
    // The perspective derivative keeps facing aligned through camera pitch/roll,
    // widescreen and off-centre players, even when the cursor is above the horizon.
    const Vec2 x = Vec2{clip[0]} - screen * clip[0].w;
    const Vec2 z = Vec2{clip[2]} - screen * clip[2].w;
    const f32 determinant = x.x * z.y - z.x * x.y;
    if (!std::isfinite(determinant) || std::abs(determinant) < kProjectionEpsilon) {
        return {};
    }
    if (glm::length(delta) < kCursorDeadZone) {
        return Vec3{0};
    }
    const Vec2 direction{(delta.x * z.y - z.x * delta.y) / determinant,
                         (x.x * delta.y - delta.x * x.y) / determinant};
    const f32 length = glm::length(direction);
    if (!std::isfinite(length) || length < kProjectionEpsilon) {
        return {};
    }
    return Vec3{direction.x / length, 0, direction.y / length};
}
MoveInput cursorRelativeMove(MoveInput move, const Vec3& position, const Vec3& aim, f32 cameraYaw) {
    const Vec3 toward = aim - position;
    if (std::hypot(toward.x, toward.z) < 0.01f) {
        return move;
    }
    const f32 heading = std::atan2(toward.x, toward.z) - cameraYaw;
    move.direction =
        Vec2{std::cos(heading) * move.direction.x + std::sin(heading) * move.direction.y,
             -std::sin(heading) * move.direction.x + std::cos(heading) * move.direction.y};
    return move;
}
bool CursorInput::update(const Input& input, const PlayBindings& bindings, bool keyboard, s32 pad) {
    const auto& pointer = input.pointer();
    const Vec2 current{pointer.x, pointer.y};
    if (!keyboard) {
        m_active = false;
        m_previous = current;
        return false;
    }
    if (input.isPadConnected(pad)) {
        for (s32 i = 0; i < static_cast<s32>(PadButton::Count); ++i) {
            if (input.isPadButtonDown(pad, static_cast<PadButton>(i))) {
                m_active = false;
            }
        }
        if (std::abs(input.padAxis(pad, PadAxis::LeftX)) > bindings.stickDeadZone ||
            std::abs(input.padAxis(pad, PadAxis::LeftY)) > bindings.stickDeadZone) {
            m_active = false;
        }
    }
    const auto keys = [&](const std::vector<Key>& bound) {
        return std::ranges::any_of(bound, [&](Key key) { return input.isKeyDown(key); });
    };
    if ((m_previous && glm::distance(*m_previous, current) > 0.0001f) || keys(bindings.up) ||
        keys(bindings.down) || keys(bindings.left) || keys(bindings.right) ||
        keys(bindings.attack) || keys(bindings.strongAttack) || pad < 0) {
        m_active = true;
    }
    m_previous = current;
    return m_active && pointer.inside;
}
} // namespace gdl::game
