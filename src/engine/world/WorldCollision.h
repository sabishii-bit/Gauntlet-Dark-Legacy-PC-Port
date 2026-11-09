#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/CanonicalDigestSink.h"
#include "engine/math/Math.h"

namespace gdl {

class NativeStateDigest;

/** One collision triangle in world space. */
struct CollisionTriangle {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    std::array<Vec3, 3> vertices{};
    s32 object = -1;     ///< the placed object it belongs to
    u32 objectFlags = 6; ///< level flags; synthetic triangles default to wall and floor queries
};

/** A swept sphere's first wall: whose face, its normal and the point met, how far along. */
struct SphereHit {
    s32 object = -1;
    u32 objectFlags = 0;
    Vec3 normal{0.0f, 1.0f, 0.0f};
    Vec3 point{0.0f, 0.0f, 0.0f};
    f32 time = 0.0f; ///< of the sweep, nought to one
};

/** A wall a body was pushed out of: whose it is and the nearest point of it. */
struct WallContact {
    s32 object = -1;
    Vec3 point{0.0f, 0.0f, 0.0f};
    bool operator==(const WallContact&) const = default;
};

void visitNativeState(NativeStateDigest& digest, const WallContact& value);

/** Where a downward probe met a floor. */
struct FloorHit {
    f32 y = 0.0f;
    Vec3 normal{0.0f, 1.0f, 0.0f};
    s32 object = -1;
    u32 objectFlags = 0;
    Vec3 point{0}; ///< actual contact on the triangle, not the probe's X/Z
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
    /** WeaponWallCollide's 0x23E less 0x200 (the secondary channel never counts as a hit):
     * a shot passes the 0x100 player-only fences and stops on floor-flagged barriers. */
    static constexpr u32 kMissileQueryFlags = 0x3E;
    /** WorldObjCollide's secondary channel, not the solid floor result. */
    static constexpr u32 kLiquidSurface = 0x200;

    /** Reads WORLDS.PS2 (or legacy collision.json), leaving
     * out the objects `layout` marks as decoration; false (with a warning) when missing or
     * malformed. */
    bool load(const std::filesystem::path& directory, const WorldLayout& layout);

    /** Takes world-space triangles and optional immutable world-object parent indices. */
    void build(std::vector<CollisionTriangle> triangles, std::span<const s32> parents = {});
    /** Adds independently removable, already world-space item surfaces. Object ids must
     * be disjoint from the level objects. Existing visibility and moving objects survive. */
    void append(std::span<const CollisionTriangle> triangles);

    /** Lets these objects' triangles follow a transform: the file keeps them local to the
     * object (as level files do for anything flagged to move, animated or not), and they
     * stand where setObjectTransform puts them, at the origin until then.
     * Decoded formats may add common query semantics (such as moving-floor
     * support); ordinary native callers preserve their authored flags. */
    void setMovingObjects(std::span<const s32> objects, u32 additionalFlags = 0);
    /** Places a moving object; others are ignored. */
    void setObjectTransform(s32 object, const Mat4& world);
    /** Whether an object's triangles block anything; all do until told otherwise. */
    void setSolid(s32 object, bool solid);
    /** The query categories an object's side can exclude: ordinary queries probe as the
     * players do, a boss's floor probe as a boss (CritterBossAI's FloorCollide category). */
    static constexpr u8 kPlayerSide = 1;
    static constexpr u8 kBossSide = 2;
    /** Native WorldObj.side excludes matching query categories, independently
     * of removal/visibility. Queries probe as kPlayerSide unless they name another. */
    void setCollisionSide(s32 object, u8 side);
    bool solid(s32 object, u8 sideMask = kPlayerSide) const;
    /** Retain wall contacts for hazards, but do not stop or push a moving body. */
    void setContactOnly(s32 object, bool contactOnly);
    bool contactOnly(s32 object) const;
    /** Temporarily prevent a rider from crossing this floor's boundary. */
    void setFloorExitBlocked(s32 object, bool blocked);
    /** Effective rider lock, including flags inherited from an animated ancestor. */
    bool floorExitBlocked(s32 object) const;
    /** Only this object's owned lock; snapshots must not materialize inherited flags. */
    bool ownFloorExitBlocked(s32 object) const;
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
                                    f32 edgeReach = 0.0f, u8 sideMask = kPlayerSide) const;
    /** The floor an item was set on (FloorPos): of the faces under the point and the edges
     * within reach, the one nearest the point's height. A ledge's edge a hand's breadth away
     * does not lift an item off the floor under it, and a landing's edge keeps a switch
     * authored at its height off a lift deck eight units down. */
    std::optional<FloorHit> restingFloorAt(const Vec3& position, f32 above, f32 below,
                                           f32 edgeReach) const;
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
     * mover just steps and then corrects. `minimumY` excludes contacts below
     * the caller's wall volume, as in sweepWalls. StaticOnly retains animated-wall contacts
     * without overlap pushout. Movement against those walls is still blocked by sweepWalls.
     */
    Vec3 resolveWalls(const Vec3& centre, f32 radius, f32 bottom, f32 top,
                      std::vector<WallContact>* contacts = nullptr,
                      std::optional<f32> minimumY = std::nullopt, WallPush push = WallPush::All,
                      u32 mask = kWallQueryFlags) const;
    /** All surface contacts of a cylinder, including sloped faces and caps.
     * Unlike wall resolution this does not approximate contact at two heights. */
    std::vector<WallContact> surfaceContacts(const Vec3& centre, f32 radius, f32 bottom,
                                             f32 top) const;
    /** Sweeps a cylinder horizontally, stopping at the first wall and sliding along it.
     * `bottom` and `top` are world-space probe heights; the destination's y is unchanged.
     * Faces block approaches from their normal side only. Opposing faces make a wall
     * solid on both sides; crossing a thin wall cannot select its far side. An optional
     * `minimumY` clips contact probes to a caller's physical collision volume. */
    Vec3 sweepWalls(const Vec3& from, const Vec3& to, f32 radius, f32 bottom, f32 top,
                    std::vector<WallContact>* contacts = nullptr,
                    std::optional<f32> minimumY = std::nullopt) const;
    /**
     * PlayerWallCollide's sphere, swept from `from` to `to` against the walls (faces within
     * the wall band of slope, approached from their front): the first it meets. A face the
     * centre starts behind never counts, as BTriLineCol has it, so nothing holds a body
     * inside a wall. Contact-only objects are left out.
     */
    std::optional<SphereHit> sweepSphere(const Vec3& from, const Vec3& to, f32 radius,
                                         u32 mask = kWallQueryFlags) const;
    /**
     * FloorCollide's sphere, swept down from `origin` by `depth`: the highest floor face
     * under the point within the sweep and the sphere's reach below it; failing a face,
     * the nearest floor edge within `radius` (CTriListCollide ranks a crossing far before
     * an edge contact). A floor above the origin is behind the sweep and never counts.
     */
    std::optional<FloorHit> floorUnder(const Vec3& origin, f32 depth, f32 radius) const;
    /**
     * Pushes PlayerWallCollide's sphere out of the walls it overlaps, as SlideAlongWall
     * corrects a step: out along each face's normal by what of the width is within its
     * plane, and only for faces the centre is in front of, so a body is never put through
     * a face it stands behind. `staticOnly` leaves the moving objects' faces alone.
     */
    Vec3 resolveSphere(const Vec3& centre, f32 radius, bool staticOnly = false,
                       u32 mask = kWallQueryFlags) const;
    /** Finite wall-query segment, approaching the stored normal's front side.
     * Uses FastWallCollide's slope filter rather than a horizontal body sweep.
     * A positive radius uses EnemyWallCollide's swept sphere, including edge contacts. */
    bool wallBetween(const Vec3& from, const Vec3& to) const;
    bool wallBetween(const Vec3& from, const Vec3& to, f32 radius) const;
    /** Nearest visible solid surface along a finite picking ray, including moving geometry. */
    std::optional<Vec3> pickSurface(const Vec3& from, const Vec3& to) const;

    class LocalSnapshot {
        friend class WorldCollision;
        std::vector<Mat4> m_transforms;
        std::vector<s32> m_hidden;
        std::vector<s32> m_contactOnly;
        std::vector<s32> m_blockedFloorExits;
        std::vector<std::pair<s32, u8>> m_collisionSides;
        const WorldCollision* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    /** Copies only live transforms and query masks, never the static triangles
     * or grid. Restore rebuilds changed moving-triangle caches in place. */
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    /** Exact captured transforms and query masks, not rebuilt triangle/grid caches. */
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    const CanonicalDigestSink::Digest& geometryDigest() const;
    std::optional<FloorHit> surfaceAt(const Vec3& position, f32 above, f32 below, bool liquid,
                                      f32 edgeReach = 0.0f, const Vec2* direction = nullptr,
                                      bool nearestToHeight = false,
                                      u8 sideMask = kPlayerSide) const;
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
        /** Transform borrowed local triangles into an equally sized, initialized
         * placed cache. Does not retain the source or change its metadata. */
        void place(const Mat4& world, std::span<const CollisionTriangle> source);
    };

    void index();
    std::vector<u32> candidates(f32 minX, f32 minZ, f32 maxX, f32 maxZ) const;
    /** Every triangle that may lie in the rectangle, still and moving, that blocks. */
    template <typename Visit>
    void eachTriangle(f32 minX, f32 minZ, f32 maxX, f32 maxZ, const Visit& visit,
                      u8 sideMask = kPlayerSide) const;

    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    std::vector<CollisionTriangle> m_triangles;
    std::vector<MovingObject> m_moving;
    std::vector<s32> m_objectParents; ///< immutable layout, included in the geometry digest
    // Geometry is immutable between build/append/setMovingObjects/clear. Local
    // history copies only masks/transforms; this cache authenticates every
    // original triangle once without scanning the level on each agreement tick.
    mutable std::optional<CanonicalDigestSink::Digest> m_geometryDigest;
    std::vector<s32> m_hidden; ///< objects told not to block
    std::vector<s32> m_contactOnly;
    std::vector<s32> m_blockedFloorExits;
    std::vector<std::pair<s32, u8>> m_collisionSides; ///< sorted by owner; zero masks omitted
    Vec3 m_min{0.0f, 0.0f, 0.0f};
    Vec3 m_max{0.0f, 0.0f, 0.0f};
    u32 m_columns = 0;
    u32 m_rows = 0;
    std::vector<std::vector<u32>> m_cells; ///< triangle indices per grid cell, row-major
};

} // namespace gdl
