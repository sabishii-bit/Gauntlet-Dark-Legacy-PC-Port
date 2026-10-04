#include "game/players/CursorAim.h"

#include <algorithm>
#include <cmath>

#include "engine/world/WorldCamera.h"

namespace gdl::game {
std::optional<Vec3> cursorAimPoint(Vec2 cursor, const Mat4& clip, const WorldCollision& collision,
                                   f32 fallbackHeight) {
    if (!std::isfinite(cursor.x) || !std::isfinite(cursor.y) || cursor.x < 0 || cursor.x > 1 ||
        cursor.y < 0 || cursor.y > 1 || std::abs(glm::determinant(clip)) < 1.0e-9f) {
        return {};
    }
    const Mat4 inverse = glm::inverse(clip);
    const auto unproject = [&](f32 depth) {
        const Vec4 point = inverse * Vec4{cursor.x * 2 - 1, cursor.y * 2 - 1, depth, 1};
        return Vec3{point} / point.w;
    };
    const Vec3 from = unproject(WorldCamera::kDepthRange);
    const Vec3 to = unproject(0);
    if (const auto hit = collision.pickSurface(from, to)) {
        return hit;
    }
    const f32 dy = to.y - from.y;
    if (std::abs(dy) < 1.0e-6f) {
        return {};
    }
    const f32 t = (fallbackHeight - from.y) / dy;
    return t >= 0 && t <= 1 ? std::optional{from + (to - from) * t} : std::nullopt;
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
