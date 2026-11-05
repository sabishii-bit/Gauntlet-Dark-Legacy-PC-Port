#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/math/PortableMath.h"
#include "engine/world/WorldCollision.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/players/PlayerControls.h"

namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {

/**
 * A character standing in a level: where it is, which way it faces and how fast it walks,
 * moved by its player's stick relative to the camera and kept on the floor and out of the
 * walls by the level's collision.
 */
class PlayerActor {
    friend void visitNativeState(NativeStateDigest& /*digest*/, const PlayerActor& /*value*/);

public:
    bool operator==(const PlayerActor&) const = default;
    static constexpr f32 kMinSpeed = 5.0f;  ///< units per second with no speed at all
    static constexpr f32 kMaxSpeed = 12.5f; ///< units per second at a speed stat of 1000
    static constexpr f32 kStatScale = 0.001f;
    static constexpr f32 kMoveLimit = 1.5f;    ///< at most this many speeds of travel per second
    static constexpr f32 kStepUp = 3.0f;       ///< the native new-floor height limit
    static constexpr f32 kDrop = 3.0f;         ///< the deepest drop walked down
    static constexpr f32 kFallSpeed = 16.0f;   ///< how fast a body sinks to a floor gone lower
    static constexpr f32 kFallReach = 1000.0f; ///< how far under it a floor is looked for
    static constexpr f32 kFootClearance = 0.2f;
    static constexpr f32 kWallSphereLift = 1.0f; ///< the wall sphere sits this over the centre
    static constexpr f32 kSecondSweep = 0.95f;   ///< of the width, for the sweep after a slide
    static constexpr f32 kSlideLimit = 1.75f;    ///< widths of correction a slide may make
    static constexpr f32 kFloorProbe = 3.0f;     ///< FloorCollide looks this and a half height down
    static constexpr f32 kSameFloorStep = 1.0f;  ///< more step on the floor already stood on
    /** PlayerCollideFloor's thousandth, widened to the port's packed-vertex seam: a
     * contact this near the point counts as under it. */
    static constexpr f32 kContactEpsilon = 1.0f / 32.0f;
    static constexpr f32 kSlopeAllowance = 4.0f; ///< a sloped floor ahead within this is fine
    static constexpr f32 kClimbFraction = 0.1f;  ///< a rise over this of the step shortens it
    static constexpr f32 kSlopeScaleFrom = 0.01f;
    static constexpr u32 kSlopedFloor = 0x8; ///< the floor flag ModifyPlayerDpos treats as slope
    // Two packed 1/64-unit offsets of contact margin across separately reconstructed
    // collision edges. Tower stair joins differ by hundredths; real gaps still stop us.
    static constexpr f32 kFloorEdgeReach = 1.0f / 32.0f;
    static constexpr f32 kDefaultHeight = 5.0f;
    static constexpr f32 kDefaultWidth = 1.5f;
    static constexpr f32 kDefaultFollowHeight = 2.5f;

    /** Stands the character for player `player` at `position` facing `yaw`, sized and paced
     * by its class (`stats` may be null for the defaults). */
    void spawn(s32 player, const CharacterSave& save, const ClassStats* stats, const Vec3& position,
               f32 yaw);

    /** Restore an isolated prediction body's host-approved motion envelope. Does not
     * restore inventory, health or any other gameplay state. */
    void restoreMotion(const Vec3& position, f32 yaw, f32 speed, f32 radius, f32 height,
                       f32 followHeight);

    /** Walks by `input`, whose forward is the camera's `cameraYaw`, for `seconds`; with
     * `collision` the step is kept on a floor and pushed out of walls. `moveScale` is how
     * much of its pace the body's action leaves it: at none it turns to the stick and stays
     * where it is. */
    void update(const MoveInput& input, f32 cameraYaw, f32 seconds, const WorldCollision* collision,
                f32 moveScale = 1.0f, bool keepFacing = false);
    /** The way `input` would send the character, as a heading about the upright. */
    static f32 headingOf(const MoveInput& input, f32 cameraYaw);

    /** Moves the character to `position`, as something solid in its way pushes it. */
    void place(const Vec3& position) {
        m_position = position;
        m_stepAim = position;
        m_floorBase = position.y;
    }
    /** Carried to `position` by the floor it rides, which it keeps standing on. */
    void carry(const Vec3& position) {
        m_floorBase += position.y - m_position.y;
        m_stepAim += position - m_position;
        m_position = position;
    }
    /** Turn in place toward an assisted attack target without moving the feet. */
    void faceToward(const Vec3& point);
    /** Faces `yaw` at once, as a hit turns the body it knocks. */
    void turnTo(f32 yaw) { m_yaw = yaw; }
    /** Carried along the ground by `offset` as a knock slides it: out of walls, and never
     * over an edge it could not walk off. */
    void slide(const Vec3& offset, const WorldCollision* collision);
    /** Follows its floor down when that has gone lower than it, sinking at most sixteen a
     * second and never faster (PlayerMotion's floor_base); a floor that rose lifts it at
     * once. True while it is above its floor. */
    bool fall(f32 seconds, const WorldCollision& collision);

    /** Drops the character onto the floor under it, when there is one. */
    void settle(const WorldCollision& collision);
    /** Takes the pace its record's class and level give it again, after the record
     * changed under it. */
    void restat(const ClassStats* stats);

    s32 player() const { return m_player; }
    const CharacterSave& save() const { return m_save; }
    CharacterSave& save() { return m_save; }
    const Vec3& position() const { return m_position; }
    /** Where the last step meant to go before walls and floors clipped it, carried along
     * with whatever moves the body afterwards. PlayerMotion sweeps items at that destination,
     * so a lever whose handle stands in a wall is still pulled from a width out. Every
     * update() writes it, so it is never checkpointed. */
    const Vec3& stepAim() const { return m_stepAim; }
    /** The floor it stands on or sinks to (floor_base), that floor's object (PlayerNewFloor's
     * floor_name2; none before it has stood anywhere) and the object's level flags. */
    f32 floorBase() const { return m_floorBase; }
    s32 floorObject() const { return m_floorObject; }
    u32 floorFlags() const { return m_floorFlags; }
    f32 yaw() const { return m_yaw; }
    /** The pace, with what a speed powerup adds, never past the class range's most. */
    f32 speed() const { return std::clamp(m_speed + m_paceBonus, kMinSpeed, kMaxSpeed); }
    /** Units a second a speed powerup adds to the class's pace. */
    void setPaceBonus(f32 bonus) { m_paceBonus = bonus; }
    f32 radius() const { return m_radius; }
    f32 height() const { return m_height; }
    /** How far the character's touch takes items: the class's whole width, as the original
     * counts it, twice the footprint that walls stop. */
    f32 reach() const { return m_radius * 2.0f; }
    /** Lowest height of the native wall-contact sphere for the given foot height.
     * Both walking and post-body correction must ignore faces wholly below it. */
    f32 minimumWallY(f32 footHeight) const;
    bool moving() const { return m_moving; }
    /** Actual wall hits accumulated over this movement tick, including slide corrections. */
    std::span<const WallContact> wallContacts() const { return m_wallContacts; }
    void clearWallContacts() { m_wallContacts.clear(); }

    /** The way the character faces, along the ground. */
    Vec3 facing() const { return Vec3{portable::sin(m_yaw), 0.0f, portable::cos(m_yaw)}; }

    /** The point the camera follows: the body's centre above the feet. */
    Vec3 followPoint() const { return m_position + Vec3{0.0f, m_followHeight, 0.0f}; }
    /** The name/attention anchor, independent of the body's collision centre. */
    Vec3 attentionPoint() const { return m_position + Vec3{0.0f, m_attentionHeight, 0.0f}; }

    /** Model space (feet at the origin, facing +z) to the world. */
    Mat4 transform() const;

private:
    /** Steps the body by `offset` along the ground, as walking and sliding both do:
     * PlayerMotion's wall sweep, then its floor rule. */
    void travel(const Vec3& offset, const WorldCollision* collision);
    static void slideAlongWall(Vec3& dpos, const Vec3& centre, const SphereHit& wall, f32 width);
    void noteContact(s32 object, const Vec3& point);

    s32 m_player = 0;
    CharacterSave m_save;
    Vec3 m_position{0.0f, 0.0f, 0.0f};
    f32 m_yaw = 0.0f;
    f32 m_speed = kMinSpeed;
    f32 m_paceBonus = 0.0f;
    f32 m_radius = kDefaultWidth * 0.5f;
    f32 m_height = kDefaultHeight;
    f32 m_followHeight = kDefaultFollowHeight;
    f32 m_attentionHeight = kDefaultFollowHeight;
    bool m_moving = false;
    Vec3 m_stepAim{0.0f, 0.0f, 0.0f}; ///< the last update's destination, before anything clipped it
    f32 m_floorBase = 0.0f;
    s32 m_floorObject = -1;
    u32 m_floorFlags = 0;
    std::vector<WallContact> m_wallContacts;
};

} // namespace gdl::game
