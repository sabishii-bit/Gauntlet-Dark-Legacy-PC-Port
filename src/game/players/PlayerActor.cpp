#include "game/players/PlayerActor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "game/players/Progression.h"

namespace gdl::game {

void PlayerActor::spawn(std::int32_t player, const CharacterSave& save, const ClassStats* stats,
                        const Vec3& position, float yaw) {
    m_player = player;
    m_save = save;
    m_position = position;
    m_yaw = yaw;
    m_moving = false;
    float speedStat = 0.0f;
    m_radius = kDefaultWidth * 0.5f;
    m_height = kDefaultHeight;
    m_followHeight = kDefaultFollowHeight;
    if (stats != nullptr) {
        const std::int32_t level = experienceLevel(save.experience());
        speedStat = static_cast<float>(displayStats(*stats, level, save.progress()).speed());
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

float PlayerActor::headingOf(const MoveInput& input, float cameraYaw) {
    return std::atan2(input.direction.x, input.direction.y) + cameraYaw;
}

void PlayerActor::update(const MoveInput& input, float cameraYaw, float seconds,
                         const WorldCollision* collision, float moveScale, bool keepFacing) {
    m_moving = input.any() && seconds > 0.0f;
    if (!m_moving) {
        return;
    }
    const float heading = headingOf(input, cameraYaw);
    const float pace = speed();
    const float distance =
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
    const auto steps =
        std::max(1, static_cast<std::int32_t>(std::ceil(distance / (m_radius * 0.5f))));
    const float stride = distance / static_cast<float>(steps);
    for (std::int32_t i = 0; i < steps; ++i) {
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
