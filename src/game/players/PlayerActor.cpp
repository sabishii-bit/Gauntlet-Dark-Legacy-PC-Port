#include "game/players/PlayerActor.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/core/Types.h"

#include "game/players/Progression.h"

namespace gdl::game {

void PlayerActor::spawn(s32 player, const CharacterSave& save, const ClassStats* stats,
                        const Vec3& position, f32 yaw) {
    m_player = player;
    m_save = save;
    m_position = position;
    m_yaw = yaw;
    m_moving = false;
    m_wallContacts.clear();
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

void PlayerActor::faceToward(const Vec3& point) {
    const Vec3 offset = point - m_position;
    if (std::hypot(offset.x, offset.z) > 1e-5f) {
        m_yaw = std::atan2(offset.x, offset.z);
    }
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
    travel(direction * distance, collision);
}

void PlayerActor::slide(const Vec3& offset, const WorldCollision* collision) {
    travel(Vec3{offset.x, 0.0f, offset.z}, collision);
}

void PlayerActor::travel(const Vec3& offset, const WorldCollision* collision) {
    const f32 distance = glm::length(offset);
    if (distance <= 0.0f) {
        return;
    }
    if (collision == nullptr) {
        m_position += offset;
        return;
    }
    // Go in steps no longer than half the body, so no wall is ever stepped clean through.
    const auto steps = std::max(1, static_cast<s32>(std::ceil(distance / (m_radius * 0.5f))));
    const Vec3 stride = offset / static_cast<f32>(steps);
    for (s32 i = 0; i < steps; ++i) {
        Vec3 target = m_position + stride;
        target = collision->sweepWalls(m_position, target, m_radius, target.y + kFootClearance,
                                       target.y + m_height - kFootClearance, &m_wallContacts);
        auto floor = collision->floorAt(target, kStepUp, kDrop, kFloorEdgeReach);
        if (!floor) {
            // Floor contact spans the body's radius, not just a ray under its
            // centre. Only a landing ahead supports crossing a seam; support
            // behind us must not allow walking away from a cliff.
            floor = collision->floorAhead(target, target - m_position, kStepUp, kDrop, m_radius);
        }
        if (!floor.has_value()) {
            const auto edge =
                collision->slideAlongFloor(m_position, target, kStepUp, kDrop, kFloorEdgeReach);
            if (!edge) {
                return;
            }
            target = collision->sweepWalls(m_position, *edge, m_radius, edge->y + kFootClearance,
                                           edge->y + m_height - kFootClearance, &m_wallContacts);
            const auto support = collision->floorAt(target, kStepUp, kDrop, kFloorEdgeReach);
            if (!support) {
                return;
            }
            target.y = support->y;
        } else {
            target.y = floor->y;
        }
        m_position = target;
    }
}

bool PlayerActor::fall(f32 seconds, const WorldCollision& collision) {
    auto floor = collision.floorAt(m_position, kStepUp, kDrop, kFloorEdgeReach);
    if (!floor) {
        floor = collision.floorAt(m_position, kStepUp, kDrop, m_radius);
    }
    if (!floor) {
        floor = collision.floorAt(m_position, kStepUp, kFallReach, kFloorEdgeReach);
    }
    if (floor.has_value() && floor->y >= m_position.y) {
        m_position.y = floor->y; // a floor that rose under it lifts it at once
        return false;
    }
    const f32 lowest = floor.has_value() ? floor->y : -std::numeric_limits<f32>::infinity();
    m_position.y = std::max(lowest, m_position.y - kFallSpeed * std::max(seconds, 0.0f));
    return true;
}

void PlayerActor::settle(const WorldCollision& collision) {
    if (const auto floor = collision.floorAt(m_position, kDrop, kDrop, kFloorEdgeReach)) {
        m_position.y = floor->y;
    }
}

Mat4 PlayerActor::transform() const {
    return glm::rotate(glm::translate(Mat4{1.0f}, m_position), m_yaw, Vec3{0.0f, 1.0f, 0.0f});
}

} // namespace gdl::game
