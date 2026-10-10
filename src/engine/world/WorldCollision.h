#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/** One collision triangle in world space. */
struct CollisionTriangle {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    std::array<Vec3, 3> vertices{};
    s32 object = -1;     ///< the placed object it belongs to
    u32 objectFlags = 6; ///< level flags; synthetic triangles default to wall and floor queries
};

/** A wall a body was pushed out of: whose it is and the nearest point of it. */
struct WallContact {
    s32 object = -1;
    Vec3 point{0.0f, 0.0f, 0.0f};
};

/** Where a downward probe met a floor. */
struct FloorHit {
    f32 y = 0.0f;
    Vec3 normal{0.0f, 1.0f, 0.0f};
    s32 object = -1;
    u32 objectFlags = 0;
};

/**
 * The walkable surfaces and walls of a level: every placed object's collision triangles in
 * world space, indexed by a grid over the ground plane. Triangles whose normal points mostly
 * up can be floors; the object's retail query flags also determine whether a surface is
 * walkable or blocks horizontal movement.
 */
class WorldCollision {
public:
    static constexpr f32 kFloorNormalY = 0.5f; ///< a normal with less y is a wall
    static constexpr f32 kCellSize = 8.0f;
    // FloorCollide / PlayerWallCollide / EnemyWallCollide pass these masks to WorldCollide.
    static constexpr u32 kFloorQueryFlags = 0x23C;
    static constexpr u32 kWallQueryFlags = 0x13A;
    /** WorldObjCollide's secondary channel, not the solid floor result. */
    static constexpr u32 kLiquidSurface = 0x200;

    /** Reads WORLDS.PS2 (or legacy collision.json), leaving
     * out the objects `layout` marks as decoration; false (with a warning) when missing or
     * malformed. */
    bool load(const std::filesystem::path& directory, const WorldLayout& layout);

    /** Takes world-space triangles directly. */
    void build(std::vector<CollisionTriangle> triangles);
    /** Adds independently removable, already world-space item surfaces. Object ids must
     * be disjoint from the level objects. Existing visibility and moving objects survive. */
    void append(std::span<const CollisionTriangle> triangles);

    /** Lets these objects' triangles follow a transform: the file keeps them local to the
     * object (as level files do for anything flagged to move, animated or not), and they
     * stand where setObjectTransform puts them, at the origin until then. */
    void setMovingObjects(std::span<const s32> objects);
    /** Places a moving object; others are ignored. */
    void setObjectTransform(s32 object, const Mat4& world);
    /** Whether an object's triangles block anything; all do until told otherwise. */
    void setSolid(s32 object, bool solid);
    bool solid(s32 object) const;
    /** Retain wall contacts for hazards, but do not stop or push a moving body. */
    void setContactOnly(s32 object, bool contactOnly);
    bool contactOnly(s32 object) const;
    /** Temporarily prevent a rider from crossing this floor's boundary. */
    void setFloorExitBlocked(s32 object, bool blocked);
    bool floorExitBlocked(s32 object) const;
    bool moving(s32 object) const;
    /** Where a moving object was last placed; none for a still one. */
    std::optional<Mat4> objectTransform(s32 object) const;
    usize movingObjectCount() const { return m_moving.size(); }

    void clear();
    bool loaded() const { return !m_triangles.empty() || !m_moving.empty(); }
    usize triangleCount() const;
    /** The lowest point of the still triangles; nought with none. */
    f32 lowest() const { return m_triangles.empty() ? 0.0f : m_min.y; }

    /** Highest floor in the vertical range. A nonzero edge reach also accepts the closest
     * point on a face within that horizontal distance, for a body's small contact margin. */
    std::optional<FloorHit> floorAt(const Vec3& position, f32 above, f32 below,
                                    f32 edgeReach = 0.0f) const;
    /** Support within a body's footprint in its direction of travel. Used when the
     * centre is over a seam; a ledge behind the body cannot support walking away. */
    std::optional<FloorHit> floorAhead(const Vec3& position, const Vec3& step, f32 above, f32 below,
                                       f32 radius) const;
    /** Project a refused short walking step onto its nearest reachable floor edge.
     * Keeps tangential movement without bridging gaps or snapping onto another storey. */
    std::optional<Vec3> slideAlongFloor(const Vec3& from, const Vec3& to, f32 above, f32 below,
                                        f32 margin = 0.0f) const;
    /** Highest secondary-channel surface in the probe range. It never supports a body. */
    std::optional<FloorHit> liquidAt(const Vec3& position, f32 above, f32 below) const;
    /** Highest solid or liquid surface for a weapon; liquids never support walking bodies. */
    std::optional<FloorHit> projectileFloorAt(const Vec3& position, f32 above, f32 below) const;

    enum class WallPush : u8 { All, StaticOnly };

    /**
     * Pushes a vertical cylinder of `radius` standing from `bottom` to `top` out of the walls
     * it overlaps and returns the corrected centre. Sliding along walls falls out of it, so a
     * mover just steps and then corrects. `minimumY` excludes faces wholly below
     * the caller's wall volume, as in sweepWalls. StaticOnly retains animated-wall contacts
     * without overlap pushout. Movement against those walls is still blocked by sweepWalls.
     */
    Vec3 resolveWalls(const Vec3& centre, f32 radius, f32 bottom, f32 top,
                      std::vector<WallContact>* contacts = nullptr,
                      std::optional<f32> minimumY = std::nullopt,
                      WallPush push = WallPush::All) const;
    /** All surface contacts of a cylinder, including sloped faces and caps.
     * Unlike wall resolution this does not approximate contact at two heights. */
    std::vector<WallContact> surfaceContacts(const Vec3& centre, f32 radius, f32 bottom,
                                             f32 top) const;
    /** Sweeps a cylinder horizontally, stopping at the first wall and sliding along it.
     * `bottom` and `top` are world-space probe heights; the destination's y is unchanged.
     * Faces block approaches from their normal side only. Opposing faces make a wall
     * solid on both sides; crossing a thin wall cannot select its far side. An optional
     * `minimumY` excludes faces wholly below a caller's physical collision volume. */
    Vec3 sweepWalls(const Vec3& from, const Vec3& to, f32 radius, f32 bottom, f32 top,
                    std::vector<WallContact>* contacts = nullptr,
                    std::optional<f32> minimumY = std::nullopt) const;
    /** Finite wall-query segment, approaching the stored normal's front side.
     * Uses FastWallCollide's slope filter rather than a horizontal body sweep. */
    bool wallBetween(const Vec3& from, const Vec3& to) const;
    /** Nearest visible solid surface along a finite picking ray, including moving geometry. */
    std::optional<Vec3> pickSurface(const Vec3& from, const Vec3& to) const;

private:
    std::optional<FloorHit> surfaceAt(const Vec3& position, f32 above, f32 below, bool liquid,
                                      f32 edgeReach = 0.0f, const Vec2* direction = nullptr) const;
    /** An object whose triangles move with it. */
    struct MovingObject {
        s32 object = -1;
        std::vector<CollisionTriangle> local;  ///< in the object's own space
        std::vector<CollisionTriangle> placed; ///< in the world, as last placed
        Vec3 boundsMin{0.0f, 0.0f, 0.0f};
        Vec3 boundsMax{0.0f, 0.0f, 0.0f};
        Mat4 world{1.0f}; ///< as last placed

        bool overlaps(f32 minX, f32 minZ, f32 maxX, f32 maxZ) const {
            return boundsMax.x >= minX && boundsMin.x <= maxX && boundsMax.z >= minZ &&
                   boundsMin.z <= maxZ;
        }
        void place(const Mat4& world);
    };

    void index();
    std::vector<u32> candidates(f32 minX, f32 minZ, f32 maxX, f32 maxZ) const;
    /** Every triangle that may lie in the rectangle, still and moving, that blocks. */
    template <typename Visit>
    void eachTriangle(f32 minX, f32 minZ, f32 maxX, f32 maxZ, const Visit& visit) const;

    std::vector<CollisionTriangle> m_triangles;
    std::vector<MovingObject> m_moving;
    std::vector<s32> m_hidden; ///< objects told not to block
    std::vector<s32> m_contactOnly;
    std::vector<s32> m_blockedFloorExits;
    Vec3 m_min{0.0f, 0.0f, 0.0f};
    Vec3 m_max{0.0f, 0.0f, 0.0f};
    u32 m_columns = 0;
    u32 m_rows = 0;
    std::vector<std::vector<u32>> m_cells; ///< triangle indices per grid cell, row-major
};

} // namespace gdl
