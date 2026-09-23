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
    u32 objectFlags = 0; ///< level flags, preserved for surface-specific collision responses
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
 * up are floors; the rest are walls.
 */
class WorldCollision {
public:
    static constexpr f32 kFloorNormalY = 0.5f; ///< a normal with less y is a wall
    static constexpr f32 kCellSize = 8.0f;

    /** Reads `directory/collision.json`, whose triangles are already in world space, leaving
     * out the objects `layout` marks as decoration; false (with a warning) when missing or
     * malformed. */
    bool load(const std::filesystem::path& directory, const WorldLayout& layout);

    /** Takes world-space triangles directly. */
    void build(std::vector<CollisionTriangle> triangles);

    /** Lets these objects' triangles follow a transform: the file keeps them local to the
     * object (as level files do for anything flagged to move, animated or not), and they
     * stand where setObjectTransform puts them, at the origin until then. */
    void setMovingObjects(std::span<const s32> objects);
    /** Places a moving object; others are ignored. */
    void setObjectTransform(s32 object, const Mat4& world);
    /** Whether an object's triangles block anything; all do until told otherwise. */
    void setSolid(s32 object, bool solid);
    bool solid(s32 object) const;
    bool moving(s32 object) const;
    usize movingObjectCount() const { return m_moving.size(); }

    void clear();
    bool loaded() const { return !m_triangles.empty() || !m_moving.empty(); }
    usize triangleCount() const;

    /** The highest floor under `position`, from `above` over it down to `below` under it. */
    std::optional<FloorHit> floorAt(const Vec3& position, f32 above, f32 below) const;

    /**
     * Pushes a vertical cylinder of `radius` standing from `bottom` to `top` out of the walls
     * it overlaps and returns the corrected centre. Sliding along walls falls out of it, so a
     * mover just steps and then corrects.
     */
    Vec3 resolveWalls(const Vec3& centre, f32 radius, f32 bottom, f32 top) const;

private:
    /** An object whose triangles move with it. */
    struct MovingObject {
        s32 object = -1;
        std::vector<CollisionTriangle> local;  ///< in the object's own space
        std::vector<CollisionTriangle> placed; ///< in the world, as last placed
        Vec3 boundsMin{0.0f, 0.0f, 0.0f};
        Vec3 boundsMax{0.0f, 0.0f, 0.0f};

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
    Vec3 m_min{0.0f, 0.0f, 0.0f};
    Vec3 m_max{0.0f, 0.0f, 0.0f};
    u32 m_columns = 0;
    u32 m_rows = 0;
    std::vector<std::vector<u32>> m_cells; ///< triangle indices per grid cell, row-major
};

} // namespace gdl
