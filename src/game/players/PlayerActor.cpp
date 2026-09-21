#include "game/players/PlayerActor.h"

#include <algorithm>
#include <cmath>

#include "game/players/Progression.h"

namespace gdl::game {

void PlayerActor::spawn(s32 player, const CharacterSave& save, const ClassStats* stats,
                        const Vec3& position, f32 yaw) {
    m_player = player;
    m_save = save;
    m_position = position;
    m_yaw = yaw;
    m_moving = false;
    f32 speedStat = 0.0f;
    m_radius = kDefaultWidth * 0.5f;
    m_height = kDefaultHeight;
    m_followHeight = kDefaultFollowHeight;
    if (stats != nullptr) {
        const s32 level = experienceLevel(save.experience());
        speedStat = static_cast<f32>(displayStats(*stats, level, save.progress()).speed());
        if (stats->width > 0.0f) {
            m_radius = stats->width * 0.5f;
        }
        if (stats->height > 0.0f) {
            m_height = stats->height;
        }
        if (stats->collisionY > 0.0f) {
            m_followHeight = stats->collisionY;
        }
    }
    m_speed = kMinSpeed + std::clamp(speedStat * kStatScale, 0.0f, 1.0f) * (kMaxSpeed - kMinSpeed);
}

f32 PlayerActor::headingOf(const MoveInput& input, f32 cameraYaw) {
    return std::atan2(input.direction.x, input.direction.y) + cameraYaw;
}

void PlayerActor::update(const MoveInput& input, f32 cameraYaw, f32 seconds,
                         const WorldCollision* collision, f32 moveScale, bool keepFacing) {
    m_moving = input.any() && seconds > 0.0f;
    if (!m_moving) {
        return;
    }
    const f32 heading = headingOf(input, cameraYaw);
    const f32 pace = speed();
    const f32 distance =
        std::min(pace * input.magnitude * seconds, kMoveLimit * pace * seconds) * moveScale;
    if (!keepFacing) {
        m_yaw = heading;
    }
    if (distance <= 0.0f) {
        m_moving = false;
        return;
    }
    const Vec3 direction{std::sin(heading), 0.0f, std::cos(heading)};
    if (collision == nullptr) {
        m_position += direction * distance;
        return;
    }
    // Walk in steps no longer than half the body, so no wall is ever stepped clean through.
    const auto steps = std::max(1, static_cast<s32>(std::ceil(distance / (m_radius * 0.5f))));
    const f32 stride = distance / static_cast<f32>(steps);
    for (s32 i = 0; i < steps; ++i) {
        Vec3 target = m_position + direction * stride;
        target = collision->resolveWalls(target, m_radius, target.y + kFootClearance,
                                         target.y + m_height - kFootClearance);
        const auto floor = collision->floorAt(target, kStepUp, kDrop);
        if (!floor.has_value()) {
            return; // nothing to stand on there: stay put
        }
        target.y = floor->y;
        m_position = target;
    }
}

void PlayerActor::settle(const WorldCollision& collision) {
    if (const auto floor = collision.floorAt(m_position, kDrop, kDrop)) {
        m_position.y = floor->y;
    }
}

Mat4 PlayerActor::transform() const {
    return glm::rotate(glm::translate(Mat4{1.0f}, m_position), m_yaw, Vec3{0.0f, 1.0f, 0.0f});
}

} // namespace gdl::game
