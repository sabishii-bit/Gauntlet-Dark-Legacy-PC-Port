#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

#include "game/players/CharacterSave.h"
#include "game/players/ClassData.h"
#include "game/players/PlayerControls.h"

namespace gdl::game {

/**
 * A character standing in a level: where it is, which way it faces and how fast it walks,
 * moved by its player's stick relative to the camera and kept on the floor and out of the
 * walls by the level's collision.
 */
class PlayerActor {
public:
    static constexpr f32 kMinSpeed = 5.0f;  ///< units per second with no speed at all
    static constexpr f32 kMaxSpeed = 12.5f; ///< units per second at a speed stat of 1000
    static constexpr f32 kStatScale = 0.001f;
    static constexpr f32 kMoveLimit = 1.5f; ///< at most this many speeds of travel per second
    static constexpr f32 kStepUp = 1.5f;    ///< the highest ledge walked up
    static constexpr f32 kDrop = 3.0f;      ///< the deepest drop walked down
    static constexpr f32 kFootClearance = 0.2f;
    static constexpr f32 kDefaultHeight = 5.0f;
    static constexpr f32 kDefaultWidth = 1.5f;
    static constexpr f32 kDefaultFollowHeight = 2.5f;

    /** Stands the character for player `player` at `position` facing `yaw`, sized and paced
     * by its class (`stats` may be null for the defaults). */
    void spawn(s32 player, const CharacterSave& save, const ClassStats* stats,
               const Vec3& position, f32 yaw);

    /** Walks by `input`, whose forward is the camera's `cameraYaw`, for `seconds`; with
     * `collision` the step is kept on a floor and pushed out of walls. */
    void update(const MoveInput& input, f32 cameraYaw, f32 seconds,
                const WorldCollision* collision);

    /** Drops the character onto the floor under it, when there is one. */
    void settle(const WorldCollision& collision);

    s32 player() const { return m_player; }
    const CharacterSave& save() const { return m_save; }
    CharacterSave& save() { return m_save; }
    const Vec3& position() const { return m_position; }
    f32 yaw() const { return m_yaw; }
    f32 speed() const { return m_speed; }
    f32 radius() const { return m_radius; }
    f32 height() const { return m_height; }
    bool moving() const { return m_moving; }

    /** The point the camera follows: the body's centre above the feet. */
    Vec3 followPoint() const { return m_position + Vec3{0.0f, m_followHeight, 0.0f}; }

    /** Model space (feet at the origin, facing +z) to the world. */
    Mat4 transform() const;

private:
    s32 m_player = 0;
    CharacterSave m_save;
    Vec3 m_position{0.0f, 0.0f, 0.0f};
    f32 m_yaw = 0.0f;
    f32 m_speed = kMinSpeed;
    f32 m_radius = kDefaultWidth * 0.5f;
    f32 m_height = kDefaultHeight;
    f32 m_followHeight = kDefaultFollowHeight;
    bool m_moving = false;
};

} // namespace gdl::game
