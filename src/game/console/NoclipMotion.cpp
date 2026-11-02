#include "game/console/NoclipMotion.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {

namespace {

/** The right stick's up and down across the pads `pad` names, -1 to 1, past the dead zone. */
f32 padRise(const Input& input, s32 pad) {
    s32 first = pad;
    s32 last = pad;
    if (pad == kAllPads) {
        first = 0;
        last = Input::kMaxPads - 1;
    } else if (pad == kNoPad) {
        last = first - 1;
    }
    f32 rise = 0.0f;
    for (s32 index = first; index <= last; ++index) {
        if (!input.isPadConnected(index)) {
            continue;
        }
        // The pad's y axis grows downwards; up is up.
        const f32 raw = -input.padAxis(index, PadAxis::RightY);
        const f32 reach = std::abs(raw);
        if (reach <= NoclipMotion::kRiseDeadZone) {
            continue;
        }
        rise += std::copysign(
            (reach - NoclipMotion::kRiseDeadZone) / (1.0f - NoclipMotion::kRiseDeadZone), raw);
    }
    return std::clamp(rise, -1.0f, 1.0f);
}

/** `axis` along the ground, a unit long; `fallback` when it points straight up or down. */
Vec3 levelled(const Vec3& axis, const Vec3& fallback) {
    const Vec3 flat{axis.x, 0.0f, axis.z};
    const f32 length = glm::length(flat);
    return length > 1.0e-4f ? flat / length : fallback;
}

} // namespace

NoclipMotion::Wish NoclipMotion::wish(const Input& input, const PlayBindings& bindings,
                                      bool keyboard, s32 pad, const WorldCamera& camera) {
    const MoveInput move = readMoveInput(input, bindings, keyboard, pad);
    const Vec3 forward = levelled(camera.forward(), Vec3{0.0f, 0.0f, 1.0f});
    const Vec3 right = levelled(camera.right(), Vec3{1.0f, 0.0f, 0.0f});
    Wish wish;
    wish.direction = (right * move.direction.x + forward * move.direction.y) * move.magnitude;
    f32 rise = padRise(input, pad);
    if (keyboard) {
        if (input.isKeyDown(Key::Q)) {
            rise += 1.0f;
        }
        if (input.isKeyDown(Key::E)) {
            rise -= 1.0f;
        }
        wish.slow = input.isKeyDown(Key::LeftShift);
    }
    wish.direction.y += std::clamp(rise, -1.0f, 1.0f);
    if (const f32 length = glm::length(wish.direction); length > 1.0f) {
        wish.direction /= length;
    }
    return wish;
}

NoclipMotion::Step NoclipMotion::advance(const Wish& wish, f32 seconds) {
    Step step;
    step.offset =
        m_velocity.advance(wish.direction * (kSpeed * (wish.slow ? kSlowFactor : 1.0f)), seconds);
    const Vec2 ground{wish.direction.x, wish.direction.z};
    if (glm::length(ground) > 1.0e-3f) {
        step.yaw = std::atan2(ground.x, ground.y);
    }
    return step;
}

} // namespace gdl::game
