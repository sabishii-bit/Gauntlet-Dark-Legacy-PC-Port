#include "engine/world/WorldCollision.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/platform/Paths.h"

#include "formats/WorldFile.h"

namespace gdl {

namespace {

using Json = nlohmann::json;

constexpr f32 kEpsilon = 0.001f;
constexpr f32 kSweepEpsilon = 1.0e-6f;
/** A centre this near a face's plane stands on it, not in front of it: a body pulled onto
 * a floor's edge where a one-sided railing stands is not held there by the railing. */
constexpr f32 kOnPlane = 0.01f;
// FastWallCollide (8000D308) accepts these inclusive normal-Y bounds;
// WorldObjCollide bypasses the normal filter for world-object flag 0x40.
constexpr f32 kRayWallNormalY = 0.866f;
constexpr u32 kUnfilteredSurface = 0x40;
constexpr s32 kPasses = 4;
/** Heights within a cylinder at which walls are checked: about the knees and the chest. */
constexpr std::array<f32, 2> kProbeFractions{0.25f, 0.75f};

bool sameTransform(const Mat4& left, const Mat4& right) {
    // Native history retains every scalar bit, including signed zero. Avoid
    // comparing object storage/padding or treating distinct retained values as
    // the same transform merely because floating-point equality does.
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            if (std::bit_cast<u32>(left[column][row]) != std::bit_cast<u32>(right[column][row])) {
                return false;
            }
        }
    }
    return true;
}

/** Barycentric bounds of a point projected onto the triangle's plane. */
bool insideFace(const CollisionTriangle& triangle, const Vec3& point) {
    const Vec3& origin = triangle.vertices[0];
    const Vec3 offset = point - origin;
    const Vec3 edge0 = triangle.vertices[1] - origin;
    const Vec3 edge1 = triangle.vertices[2] - origin;
    const f32 d00 = glm::dot(edge0, edge0);
    const f32 d01 = glm::dot(edge0, edge1);
    const f32 d11 = glm::dot(edge1, edge1);
    const f32 determinant = d00 * d11 - d01 * d01;
    if (determinant <= 0) {
        return false;
    }
    const f32 d20 = glm::dot(offset, edge0);
    const f32 d21 = glm::dot(offset, edge1);
    const f32 u = (d11 * d20 - d01 * d21) / determinant;
    const f32 v = (d00 * d21 - d01 * d20) / determinant;
    return u >= 0 && v >= 0 && u + v <= 1;
}

/** A one-sided finite plane crossing followed by the triangle's barycentric bounds. */
bool crossesFront(const CollisionTriangle& triangle, const Vec3& from, const Vec3& to) {
    const Vec3& origin = triangle.vertices[0];
    const f32 start = glm::dot(from - origin, triangle.normal);
    const f32 end = glm::dot(to - origin, triangle.normal);
    if (start < 0 || end > 0) {
        return false;
    }
    // TriLineCol (8002109C) tests the starting point for a coplanar segment,
    // including a stationary contact. Its stored normal, not vertex winding,
    // determines the blocking side.
    const f32 fraction = start > end ? start / (start - end) : 0;
    return insideFace(triangle, from + (to - from) * fraction);
}

/** Squared distance of two finite 3D segments, including stationary endpoints. */
f32 segmentDistanceSquared(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d) {
    const Vec3 u = b - a;
    const Vec3 v = d - c;
    const Vec3 offset = a - c;
    const f32 uu = glm::dot(u, u);
    const f32 vv = glm::dot(v, v);
    const f32 uv = glm::dot(u, v);
    const f32 ur = glm::dot(u, offset);
    const f32 vr = glm::dot(v, offset);
    f32 s = 0;
    f32 t = 0;
    if (uu <= kSweepEpsilon) {
        t = vv > kSweepEpsilon ? std::clamp(vr / vv, 0.0f, 1.0f) : 0;
    } else if (vv <= kSweepEpsilon) {
        s = std::clamp(-ur / uu, 0.0f, 1.0f);
    } else {
        const f32 determinant = uu * vv - uv * uv;
        if (determinant > kSweepEpsilon) {
            s = std::clamp((uv * vr - vv * ur) / determinant, 0.0f, 1.0f);
        }
        t = (uv * s + vr) / vv;
        if (t < 0) {
            t = 0;
            s = std::clamp(-ur / uu, 0.0f, 1.0f);
        } else if (t > 1) {
            t = 1;
            s = std::clamp((uv - ur) / uu, 0.0f, 1.0f);
        }
    }
    const Vec3 distance = offset + s * u - t * v;
    return glm::dot(distance, distance);
}

/** BTriLineCol (800213E0): front-side, non-separating sphere/triangle sweep. */
bool sweepsFront(const CollisionTriangle& triangle, const Vec3& from, const Vec3& to, f32 radius) {
    const Vec3& origin = triangle.vertices[0];
    const f32 start = glm::dot(from - origin, triangle.normal);
    const f32 end = glm::dot(to - origin, triangle.normal);
    if (start < 0 || end > start || end > radius) {
        return false;
    }
    if (crossesFront(triangle, from, to)) {
        return true;
    }
    if ((start <= radius && insideFace(triangle, from - start * triangle.normal)) ||
        (std::abs(end) <= radius && insideFace(triangle, to - end * triangle.normal))) {
        return true;
    }
    for (usize i = 0; i < triangle.vertices.size(); ++i) {
        if (segmentDistanceSquared(from, to, triangle.vertices[i],
                                   triangle.vertices[(i + 1) % triangle.vertices.size()]) <=
            radius * radius) {
            return true;
        }
    }
    return false;
}

/** Whether (x, z) lies inside the triangle's ground-plane projection. */
bool insideXZ(const CollisionTriangle& triangle, f32 x, f32 z) {
    const auto side = [&](const Vec3& a, const Vec3& b) {
        return (b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x);
    };
    const f32 s0 = side(triangle.vertices[0], triangle.vertices[1]);
    const f32 s1 = side(triangle.vertices[1], triangle.vertices[2]);
    const f32 s2 = side(triangle.vertices[2], triangle.vertices[0]);
    const bool negative = s0 < -kEpsilon || s1 < -kEpsilon || s2 < -kEpsilon;
    const bool positive = s0 > kEpsilon || s1 > kEpsilon || s2 > kEpsilon;
    return !(negative && positive);
}

/** The line where a triangle crosses the horizontal plane at `y`, if it does. */
struct Slice {
    Vec2 a{0.0f, 0.0f};
    Vec2 b{0.0f, 0.0f};
    bool valid = false;
};

Slice sliceAt(const CollisionTriangle& triangle, f32 y) {
    std::array<Vec2, 6> points{};
    usize count = 0;
    for (usize i = 0; i < 3; ++i) {
        const Vec3& p = triangle.vertices[i];
        const Vec3& q = triangle.vertices[(i + 1) % 3];
        if (p.y == q.y) {
            if (p.y == y) {
                points[count++] = Vec2{p.x, p.z};
                points[count++] = Vec2{q.x, q.z};
            }
            continue;
        }
        if ((p.y - y) * (q.y - y) > 0.0f) {
            continue;
        }
        const f32 t = (y - p.y) / (q.y - p.y);
        points[count++] = Vec2{p.x + (q.x - p.x) * t, p.z + (q.z - p.z) * t};
    }
    Slice slice;
    f32 longest = kEpsilon * kEpsilon;
    for (usize i = 0; i < count; ++i) {
        for (usize j = i + 1; j < count; ++j) {
            const f32 length = glm::dot(points[j] - points[i], points[j] - points[i]);
            if (length > longest) {
                longest = length;
                slice = Slice{points[i], points[j], true};
            }
        }
    }
    return slice;
}

Vec2 closestOnSegment(const Vec2& a, const Vec2& b, const Vec2& point) {
    const Vec2 edge = b - a;
    const f32 length = glm::dot(edge, edge);
    if (length <= kEpsilon * kEpsilon) {
        return a;
    }
    const f32 t = std::clamp(glm::dot(point - a, edge) / length, 0.0f, 1.0f);
    return a + edge * t;
}

struct WallSweep {
    f32 time = 1.0f;
    Vec2 normal{0};
    bool hit = false;
    WallContact contact;
};

/** The first contact of a moving circle with a segment and its round ends. */
void sweepSegment(const Vec2& from, const Vec2& step, const Slice& slice, f32 radius, s32 object,
                  f32 height, WallSweep& nearest) {
    const auto accept = [&](f32 time, const Vec2& normal) {
        if (time >= 0 && time <= nearest.time && glm::dot(step, normal) < -kSweepEpsilon) {
            const Vec2 point = from + time * step - normal * radius;
            nearest = WallSweep{time, normal, true, {object, {point.x, height, point.y}}};
        }
    };
    const Vec2 away = from - closestOnSegment(slice.a, slice.b, from);
    const f32 distance = glm::length(away);
    if (distance > kSweepEpsilon && distance <= radius) {
        accept(0, away / distance);
        // A body already overlapping can move out, but cannot move further into this side.
        return;
    }
    const Vec2 edge = slice.b - slice.a;
    const f32 length = glm::length(edge);
    const Vec2 tangent = edge / length;
    const Vec2 normal{-tangent.y, tangent.x};
    const f32 normalSpeed = glm::dot(step, normal);
    if (distance <= kSweepEpsilon) {
        accept(0, normalSpeed > 0 ? -normal : normal);
        return;
    }
    if (std::abs(normalSpeed) > kSweepEpsilon) {
        for (const f32 side : {-1.0f, 1.0f}) {
            const f32 time = (side * radius - glm::dot(from - slice.a, normal)) / normalSpeed;
            const f32 along = glm::dot(from + time * step - slice.a, tangent);
            if (along >= 0 && along <= length) {
                accept(time, side * normal);
            }
        }
    }
    const f32 speedSquared = glm::dot(step, step);
    for (const Vec2& end : {slice.a, slice.b}) {
        const Vec2 offset = from - end;
        const f32 projected = glm::dot(offset, step);
        const f32 discriminant =
            projected * projected - speedSquared * (glm::dot(offset, offset) - radius * radius);
        if (discriminant >= 0) {
            const f32 time = (-projected - std::sqrt(discriminant)) / speedSquared;
            const Vec2 contact = from + time * step - end;
            const f32 reach = glm::length(contact);
            if (reach > kEpsilon) {
                accept(time, contact / reach);
            }
        }
    }
}

Vec3 readVec3(const Json& array, usize first) {
    return Vec3{array.at(first).get<f32>(), array.at(first + 1).get<f32>(),
                array.at(first + 2).get<f32>()};
}

} // namespace

bool WorldCollision::load(const std::filesystem::path& directory, const WorldLayout& layout) {
    clear();
    std::vector<s32> parents;
    parents.reserve(layout.objects().size());
    for (const auto& object : layout.objects()) {
        parents.push_back(object.parent);
    }
    const std::filesystem::path file = directory / "collision.json";
    try {
        if (const auto native = AssetLocator(directory).find("worlds.ps2")) {
            const auto world = formats::WorldFile::parse(readFile(*native));
            if (world.objects.size() != layout.objects().size()) {
                throw FormatError("collision and layout object rosters differ");
            }
            std::vector<CollisionTriangle> triangles;
            for (usize i = 0; i < world.objects.size(); ++i) {
                const auto& object = world.objects[i];
                if (layout.objects()[i].noCollision || object.collisionTriangleCount <= 0 ||
                    object.collisionTriangleIndex < 0) {
                    continue;
                }
                const auto first = static_cast<usize>(object.collisionTriangleIndex);
                const auto count = static_cast<usize>(object.collisionTriangleCount);
                if (first + count > world.collision.size()) {
                    throw FormatError("object collision outside world triangle table");
                }
                for (usize t = first; t < first + count; ++t) {
                    const auto& from = world.collision[t];
                    triangles.push_back({from.normal, from.vertices, static_cast<s32>(i),
                                         layout.objects()[i].flags});
                }
            }
            build(std::move(triangles), parents);
            return loaded();
        }
        const Json root = Json::parse(readTextFile(file), nullptr, true, true);
        std::vector<CollisionTriangle> triangles;
        for (const Json& entry : root.at("objects")) {
            const auto object = entry.at("object").get<s32>();
            if (object < 0 || static_cast<usize>(object) >= layout.objects().size()) {
                throw FormatError("object index out of range");
            }
            if (layout.objects()[static_cast<usize>(object)].noCollision) {
                continue;
            }
            const Json& normals = entry.at("normals");
            const Json& vertices = entry.at("vertices");
            if (!normals.is_array() || !vertices.is_array() || normals.size() % 3 != 0 ||
                vertices.size() != normals.size() * 3) {
                throw FormatError("malformed triangle lists");
            }
            const usize count = normals.size() / 3;
            for (usize t = 0; t < count; ++t) {
                CollisionTriangle triangle;
                triangle.object = object;
                triangle.objectFlags = layout.objects()[static_cast<usize>(object)].flags;
                triangle.normal = readVec3(normals, t * 3);
                for (usize k = 0; k < 3; ++k) {
                    triangle.vertices[k] = readVec3(vertices, (t * 3 + k) * 3);
                }
                triangles.push_back(triangle);
            }
        }
        build(std::move(triangles), parents);
    } catch (const std::exception& e) {
        log::warn("World collision: cannot read {}: {}", paths::utf8(file), e.what());
        clear();
        return false;
    }
    return loaded();
}

const CanonicalDigestSink::Digest& WorldCollision::geometryDigest() const {
    if (!m_geometryDigest) {
        NativeStateDigest geometry;
        geometry.string("WorldCollision.geometry/2");
        geometry.value(m_objectParents);
        const auto triangles = [&](const auto& values) {
            geometry.wide(values.size());
            for (const auto& value : values) {
                geometry.fields<4>(value);
            }
        };
        triangles(m_triangles);
        geometry.wide(m_moving.size());
        for (const auto& mover : m_moving) {
            geometry.value(mover.object);
            triangles(mover.local);
        }
        m_geometryDigest = geometry.finish();
    }
    return *m_geometryDigest;
}

bool WorldCollision::appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const {
    digest.string("WorldCollision/3");
    digest.bytes(geometryDigest());
    for (usize i = 0; i < m_moving.size(); ++i) {
        const auto& [object, local, placed, boundsMin, boundsMax, world] = m_moving[i];
        digest.value(saved ? saved->m_transforms[i] : world);
        (void)object; // Complete immutable roster is in geometryDigest().
        (void)local;
        // Placed triangles/bounds are derived by MovingObject::place from these
        // exact local triangles and transform; the static grid derives from m_triangles.
        (void)placed;
        (void)boundsMin;
        (void)boundsMax;
    }
    digest.values(saved ? saved->m_hidden : m_hidden, saved ? saved->m_contactOnly : m_contactOnly,
                  saved ? saved->m_blockedFloorExits : m_blockedFloorExits,
                  saved ? saved->m_collisionSides : m_collisionSides);
    return digest.good();
}

bool WorldCollision::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, nullptr);
}

bool WorldCollision::appendLocalDigest(NativeStateDigest& digest,
                                       const LocalSnapshot& saved) const {
    // Guard the native snapshot roster too; owner/epoch are admission, not peer state.
    [[maybe_unused]] const auto& [transforms, hidden, contactOnly, blockedFloorExits,
                                  collisionSides, owner, binding] = saved;
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, &saved);
}

void WorldCollision::saveLocal(LocalSnapshot& saved) const {
    saved.m_transforms.resize(m_moving.size());
    for (usize i = 0; i < m_moving.size(); ++i) {
        saved.m_transforms[i] = m_moving[i].world;
    }
    saved.m_hidden = m_hidden;
    saved.m_contactOnly = m_contactOnly;
    saved.m_blockedFloorExits = m_blockedFloorExits;
    saved.m_collisionSides = m_collisionSides;
    saved.m_owner = this;
    saved.m_binding = m_localBinding;
}

bool WorldCollision::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_localBinding && saved.m_binding.lock() == m_localBinding &&
           saved.m_transforms.size() == m_moving.size();
}

bool WorldCollision::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) &&
           left.m_transforms == right.m_transforms && left.m_hidden == right.m_hidden &&
           left.m_contactOnly == right.m_contactOnly &&
           left.m_blockedFloorExits == right.m_blockedFloorExits &&
           left.m_collisionSides == right.m_collisionSides;
}

bool WorldCollision::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    for (usize i = 0; i < m_moving.size(); ++i) {
        if (!sameTransform(m_moving[i].world, saved.m_transforms[i])) {
            m_moving[i].place(saved.m_transforms[i]);
        }
    }
    m_hidden = saved.m_hidden;
    m_contactOnly = saved.m_contactOnly;
    m_blockedFloorExits = saved.m_blockedFloorExits;
    m_collisionSides = saved.m_collisionSides;
    return true;
}

void WorldCollision::build(std::vector<CollisionTriangle> triangles, std::span<const s32> parents) {
    clear();
    m_triangles = std::move(triangles);
    m_objectParents.assign(parents.begin(), parents.end());
    index();
}

void WorldCollision::append(std::span<const CollisionTriangle> triangles) {
    m_localBinding = std::make_shared<const u8>(0);
    m_geometryDigest.reset();
    m_triangles.insert(m_triangles.end(), triangles.begin(), triangles.end());
    index();
}

void WorldCollision::MovingObject::place(const Mat4& world) {
    place(world, local);
}

void WorldCollision::MovingObject::place(const Mat4& world,
                                         std::span<const CollisionTriangle> source) {
    this->world = world;
    const Mat3 rotation{world};
    bool first = true;
    for (usize i = 0; i < source.size(); ++i) {
        const CollisionTriangle& from = source[i];
        CollisionTriangle& to = placed[i];
        const Vec3 normal = rotation * from.normal;
        // Authored explosion planes (for example L1's L1CFXP_EXPLODE) shrink to
        // zero between bursts. Their collapsed surface has no normal or contact;
        // keep the original local normal for the next nonzero animation frame.
        to.normal = glm::dot(normal, normal) == 0 ? Vec3{0} : glm::normalize(normal);
        for (usize k = 0; k < 3; ++k) {
            to.vertices[k] = Vec3{world * Vec4{from.vertices[k], 1.0f}};
            boundsMin = first ? to.vertices[k] : glm::min(boundsMin, to.vertices[k]);
            boundsMax = first ? to.vertices[k] : glm::max(boundsMax, to.vertices[k]);
            first = false;
        }
    }
}

void WorldCollision::setMovingObjects(std::span<const s32> objects, u32 additionalFlags) {
    m_localBinding = std::make_shared<const u8>(0);
    m_geometryDigest.reset();
    for (const s32 object : objects) {
        if (moving(object)) {
            continue;
        }
        MovingObject mover;
        mover.object = object;
        for (CollisionTriangle triangle : m_triangles) {
            if (triangle.object == object) {
                triangle.objectFlags |= additionalFlags;
                mover.local.push_back(triangle);
                mover.placed.push_back(triangle);
            }
        }
        if (mover.local.empty()) {
            continue;
        }
        std::erase_if(m_triangles, [&](const CollisionTriangle& t) { return t.object == object; });
        mover.place(Mat4{1.0f});
        m_moving.push_back(std::move(mover));
    }
    m_cells.clear();
    index();
}

void WorldCollision::setObjectTransform(s32 object, const Mat4& world) {
    for (MovingObject& mover : m_moving) {
        if (mover.object == object) {
            // World and trigger phases may synchronize the same placement more
            // than once. The initial identity is already placed when this mover
            // is constructed; its local triangles never change afterward.
            if (!sameTransform(mover.world, world)) {
                mover.place(world);
            }
            return;
        }
    }
}

void WorldCollision::setSolid(s32 object, bool solid) {
    const auto found = std::ranges::find(m_hidden, object);
    if (solid && found != m_hidden.end()) {
        m_hidden.erase(found);
    } else if (!solid && found == m_hidden.end()) {
        m_hidden.push_back(object);
    }
}

void WorldCollision::setFloorExitBlocked(s32 object, bool blocked) {
    const auto found = std::ranges::find(m_blockedFloorExits, object);
    if (!blocked && found != m_blockedFloorExits.end()) {
        m_blockedFloorExits.erase(found);
    } else if (blocked && object >= 0 && found == m_blockedFloorExits.end()) {
        m_blockedFloorExits.push_back(object);
    }
}

bool WorldCollision::floorExitBlocked(s32 object) const {
    if (object < 0 || !solid(object)) {
        return false;
    }
    // PlayerCollideFloor queries WorldObjGetAllFlags, not just the contact
    // triangle's object. K4's elevator is animated root 231, with floor 232.
    for (usize depth = 0; depth <= m_objectParents.size(); ++depth) {
        if (ownFloorExitBlocked(object)) {
            return true;
        }
        if (object < 0 || static_cast<usize>(object) >= m_objectParents.size()) {
            break;
        }
        object = m_objectParents[static_cast<usize>(object)];
    }
    return false;
}

bool WorldCollision::ownFloorExitBlocked(s32 object) const {
    return object >= 0 && solid(object) &&
           std::ranges::find(m_blockedFloorExits, object) != m_blockedFloorExits.end();
}

void WorldCollision::setCollisionSide(s32 object, u8 side) {
    if (object < 0)
        return;
    const auto found =
        std::ranges::lower_bound(m_collisionSides, object, {}, &std::pair<s32, u8>::first);
    if (found != m_collisionSides.end() && found->first == object) {
        if (side == 0)
            m_collisionSides.erase(found);
        else
            found->second = side;
    } else if (side != 0) {
        m_collisionSides.insert(found, {object, side});
    }
}

bool WorldCollision::solid(s32 object, u8 sideMask) const {
    if (std::ranges::find(m_hidden, object) != m_hidden.end())
        return false;
    const auto found =
        std::ranges::lower_bound(m_collisionSides, object, {}, &std::pair<s32, u8>::first);
    return found == m_collisionSides.end() || found->first != object ||
           (found->second & sideMask) == 0;
}

bool WorldCollision::moving(s32 object) const {
    return std::ranges::any_of(m_moving,
                               [&](const MovingObject& mover) { return mover.object == object; });
}

void WorldCollision::setContactOnly(s32 object, bool contactOnly) {
    if (contactOnly) {
        if (!this->contactOnly(object)) {
            m_contactOnly.push_back(object);
        }
    } else {
        std::erase(m_contactOnly, object);
    }
}

bool WorldCollision::contactOnly(s32 object) const {
    return std::ranges::find(m_contactOnly, object) != m_contactOnly.end();
}

std::optional<Mat4> WorldCollision::objectTransform(s32 object) const {
    const auto found = std::ranges::find_if(
        m_moving, [&](const MovingObject& mover) { return mover.object == object; });
    return found != m_moving.end() ? std::optional<Mat4>{found->world} : std::nullopt;
}

usize WorldCollision::triangleCount() const {
    usize count = m_triangles.size();
    for (const MovingObject& mover : m_moving) {
        count += mover.local.size();
    }
    return count;
}

template <typename Visit>
void WorldCollision::eachTriangle(f32 minX, f32 minZ, f32 maxX, f32 maxZ, const Visit& visit,
                                  u8 sideMask) const {
    for (const u32 index : candidates(minX, minZ, maxX, maxZ)) {
        const CollisionTriangle& triangle = m_triangles[index];
        if (solid(triangle.object, sideMask)) {
            visit(triangle);
        }
    }
    for (const MovingObject& mover : m_moving) {
        if (!mover.overlaps(minX, minZ, maxX, maxZ) || !solid(mover.object, sideMask)) {
            continue;
        }
        for (const CollisionTriangle& triangle : mover.placed) {
            if (triangle.normal != Vec3{0}) {
                visit(triangle);
            }
        }
    }
}

void WorldCollision::clear() {
    m_localBinding = std::make_shared<const u8>(0);
    m_geometryDigest.reset();
    m_triangles.clear();
    m_moving.clear();
    m_objectParents.clear();
    m_hidden.clear();
    m_contactOnly.clear();
    m_blockedFloorExits.clear();
    m_collisionSides.clear();
    m_cells.clear();
    m_columns = 0;
    m_rows = 0;
}

/** Sorts every triangle into the grid cells its footprint touches. */
void WorldCollision::index() {
    if (m_triangles.empty()) {
        return;
    }
    m_min = m_triangles[0].vertices[0];
    m_max = m_min;
    for (const CollisionTriangle& triangle : m_triangles) {
        for (const Vec3& v : triangle.vertices) {
            m_min = glm::min(m_min, v);
            m_max = glm::max(m_max, v);
        }
    }
    m_columns = static_cast<u32>(std::ceil((m_max.x - m_min.x) / kCellSize)) + 1;
    m_rows = static_cast<u32>(std::ceil((m_max.z - m_min.z) / kCellSize)) + 1;
    m_cells.assign(usize{m_columns} * m_rows, {});
    const auto column = [&](f32 x) {
        return static_cast<u32>(
            std::clamp((x - m_min.x) / kCellSize, 0.0f, static_cast<f32>(m_columns - 1)));
    };
    const auto row = [&](f32 z) {
        return static_cast<u32>(
            std::clamp((z - m_min.z) / kCellSize, 0.0f, static_cast<f32>(m_rows - 1)));
    };
    for (usize i = 0; i < m_triangles.size(); ++i) {
        const auto& v = m_triangles[i].vertices;
        const f32 minX = std::min({v[0].x, v[1].x, v[2].x});
        const f32 maxX = std::max({v[0].x, v[1].x, v[2].x});
        const f32 minZ = std::min({v[0].z, v[1].z, v[2].z});
        const f32 maxZ = std::max({v[0].z, v[1].z, v[2].z});
        for (u32 r = row(minZ); r <= row(maxZ); ++r) {
            for (u32 c = column(minX); c <= column(maxX); ++c) {
                m_cells[usize{r} * m_columns + c].push_back(static_cast<u32>(i));
            }
        }
    }
}

std::vector<u32> WorldCollision::candidates(f32 minX, f32 minZ, f32 maxX, f32 maxZ) const {
    std::vector<u32> out;
    if (m_cells.empty()) {
        return out;
    }
    const auto column = [&](f32 x) {
        return static_cast<u32>(
            std::clamp((x - m_min.x) / kCellSize, 0.0f, static_cast<f32>(m_columns - 1)));
    };
    const auto row = [&](f32 z) {
        return static_cast<u32>(
            std::clamp((z - m_min.z) / kCellSize, 0.0f, static_cast<f32>(m_rows - 1)));
    };
    for (u32 r = row(minZ); r <= row(maxZ); ++r) {
        for (u32 c = column(minX); c <= column(maxX); ++c) {
            const std::vector<u32>& cell = m_cells[usize{r} * m_columns + c];
            out.insert(out.end(), cell.begin(), cell.end());
        }
    }
    std::ranges::sort(out);
    const auto duplicates = std::ranges::unique(out);
    out.erase(duplicates.begin(), duplicates.end());
    return out;
}

std::optional<FloorHit> WorldCollision::floorAt(const Vec3& position, f32 above, f32 below,
                                                f32 edgeReach, u8 sideMask) const {
    return surfaceAt(position, above, below, false, std::max(0.0f, edgeReach), nullptr, false,
                     sideMask);
}

std::optional<FloorHit> WorldCollision::restingFloorAt(const Vec3& position, f32 above, f32 below,
                                                       f32 edgeReach) const {
    return surfaceAt(position, above, below, false, std::max(0.0f, edgeReach), nullptr, true);
}

std::optional<FloorHit> WorldCollision::liquidAt(const Vec3& position, f32 above, f32 below) const {
    return surfaceAt(position, above, below, true);
}

std::optional<FloorHit> WorldCollision::floorAhead(const Vec3& position, const Vec3& step,
                                                   f32 above, f32 below, f32 radius) const {
    const Vec2 direction{step.x, step.z};
    if (glm::dot(direction, direction) <= kEpsilon * kEpsilon) {
        return std::nullopt;
    }
    return surfaceAt(position, above, below, false, std::max(0.0f, radius), &direction);
}

std::optional<Vec3> WorldCollision::slideAlongFloor(const Vec3& from, const Vec3& to, f32 above,
                                                    f32 below, f32 margin) const {
    if (!floorAt(from, above, below, margin)) {
        return std::nullopt; // a floor removed from under the body is a fall, not an edge slide
    }
    const Vec2 step{to.x - from.x, to.z - from.z};
    const f32 reach = glm::length(step);
    if (reach <= kEpsilon) {
        return std::nullopt;
    }
    std::optional<Vec3> best;
    f32 bestDistance = reach * reach;
    eachTriangle(
        to.x - reach, to.z - reach, to.x + reach, to.z + reach,
        [&](const CollisionTriangle& triangle) {
            if ((triangle.objectFlags & kFloorQueryFlags) == 0 ||
                (triangle.objectFlags & kLiquidSurface) != 0 || triangle.normal.y < kFloorNormalY) {
                return;
            }
            for (usize i = 0; i < triangle.vertices.size(); ++i) {
                const auto& a = triangle.vertices[i];
                const auto& b = triangle.vertices[(i + 1) % triangle.vertices.size()];
                const Vec2 point = closestOnSegment({a.x, a.z}, {b.x, b.z}, {to.x, to.z});
                const Vec2 correction = point - Vec2{to.x, to.z};
                const f32 distance = glm::dot(correction, correction);
                const Vec2 travel = point - Vec2{from.x, from.z};
                if (distance >= bestDistance || glm::dot(travel, step) <= 0 ||
                    glm::length(travel) > reach + kEpsilon) {
                    continue;
                }
                const f32 y = a.y - (triangle.normal.x * (point.x - a.x) +
                                     triangle.normal.z * (point.y - a.z)) /
                                        triangle.normal.y;
                if (y > from.y + above || y < from.y - below) {
                    continue;
                }
                // A nearby disconnected platform is not a route around the edge.
                // Test the whole short path at the same contact margin used for steps.
                const f32 spacing = std::max(margin, kEpsilon);
                const auto samples =
                    std::max(1, static_cast<s32>(std::ceil(glm::length(travel) / spacing)));
                bool connected = true;
                for (s32 sample = 1; sample <= samples; ++sample) {
                    const Vec2 along = Vec2{from.x, from.z} + travel * (static_cast<f32>(sample) /
                                                                        static_cast<f32>(samples));
                    if (!floorAt({along.x, from.y, along.y}, above, below, margin)) {
                        connected = false;
                        break;
                    }
                }
                if (connected) {
                    best = Vec3{point.x, y, point.y};
                    bestDistance = distance;
                }
            }
        });
    return best;
}

std::optional<FloorHit> WorldCollision::projectileFloorAt(const Vec3& position, f32 above,
                                                          f32 below) const {
    const auto floor = floorAt(position, above, below);
    const auto liquid = liquidAt(position, above, below);
    return liquid && (!floor || liquid->y > floor->y) ? liquid : floor;
}

std::optional<FloorHit> WorldCollision::surfaceAt(const Vec3& position, f32 above, f32 below,
                                                  bool liquid, f32 edgeReach, const Vec2* direction,
                                                  bool nearestToHeight, u8 sideMask) const {
    std::optional<FloorHit> best;
    const f32 highest = position.y + above;
    const f32 lowest = position.y - below;
    eachTriangle(
        position.x - edgeReach, position.z - edgeReach, position.x + edgeReach,
        position.z + edgeReach,
        [&](const CollisionTriangle& triangle) {
            if ((triangle.objectFlags & kFloorQueryFlags) == 0 ||
                ((triangle.objectFlags & kLiquidSurface) != 0) != liquid ||
                triangle.normal.y < kFloorNormalY) {
                return;
            }
            Vec2 point{position.x, position.z};
            if (!insideXZ(triangle, point.x, point.y)) {
                if (edgeReach <= 0.0f) {
                    return;
                }
                f32 nearest = edgeReach * edgeReach;
                bool found = false;
                for (usize i = 0; i < triangle.vertices.size(); ++i) {
                    const Vec3& a = triangle.vertices[i];
                    const Vec3& b = triangle.vertices[(i + 1) % triangle.vertices.size()];
                    const Vec2 edge =
                        closestOnSegment({a.x, a.z}, {b.x, b.z}, {position.x, position.z});
                    const Vec2 delta = edge - Vec2{position.x, position.z};
                    const f32 distance = glm::dot(delta, delta);
                    if (distance <= nearest) {
                        nearest = distance;
                        point = edge;
                        found = true;
                    }
                }
                if (!found) {
                    return;
                }
            }
            const Vec3& v = triangle.vertices[0];
            if (direction != nullptr &&
                glm::dot(point - Vec2{position.x, position.z}, *direction) < 0) {
                return;
            }
            const f32 y =
                v.y - (triangle.normal.x * (point.x - v.x) + triangle.normal.z * (point.y - v.z)) /
                          triangle.normal.y;
            if (y > highest || y < lowest) {
                return;
            }
            const bool better =
                !best.has_value() ||
                (nearestToHeight ? std::abs(y - position.y) < std::abs(best->y - position.y)
                                 : y > best->y);
            if (better) {
                best = FloorHit{y,
                                triangle.normal,
                                triangle.object,
                                triangle.objectFlags,
                                {point.x, y, point.y}};
            }
        },
        sideMask);
    return best;
}

Vec3 WorldCollision::sweepWalls(const Vec3& from, const Vec3& to, f32 radius, f32 bottom, f32 top,
                                std::vector<WallContact>* contacts,
                                std::optional<f32> minimumY) const {
    Vec2 position{from.x, from.z};
    Vec2 remaining{to.x - from.x, to.z - from.z};
    for (s32 pass = 0; pass < kPasses && glm::length(remaining) > kSweepEpsilon; ++pass) {
        WallSweep nearest;
        const Vec2 destination = position + remaining;
        eachTriangle(
            std::min(position.x, destination.x) - radius,
            std::min(position.y, destination.y) - radius,
            std::max(position.x, destination.x) + radius,
            std::max(position.y, destination.y) + radius, [&](const CollisionTriangle& triangle) {
                if (contactOnly(triangle.object) || (triangle.objectFlags & kWallQueryFlags) == 0 ||
                    (triangle.objectFlags & kLiquidSurface) != 0 ||
                    std::abs(triangle.normal.y) >= kFloorNormalY) {
                    return;
                }
                if (minimumY && std::ranges::all_of(triangle.vertices, [&](const Vec3& point) {
                        return point.y < *minimumY;
                    })) {
                    return;
                }
                const Vec3 step{remaining.x, 0, remaining.y};
                if (glm::dot(step, triangle.normal) > 0) {
                    return; // separating from the solid face
                }
                for (const f32 fraction : kProbeFractions) {
                    // The native player sphere cannot hit below minimumY.
                    // A triangle's high far end passing the broad-phase test
                    // must not let its low near end collide with a knee probe
                    // outside that volume (e.g. Ice Camp's sloping rails).
                    const f32 height =
                        std::max(bottom + (top - bottom) * fraction, minimumY.value_or(bottom));
                    if (height > top) {
                        continue;
                    }
                    const Vec3 origin{position.x, height, position.y};
                    if (glm::dot(origin - triangle.vertices[0], triangle.normal) < 0) {
                        continue; // entering through the back face is allowed
                    }
                    const Slice slice = sliceAt(triangle, height);
                    if (slice.valid) {
                        sweepSegment(position, remaining, slice, radius, triangle.object, height,
                                     nearest);
                    }
                }
            });
        if (!nearest.hit) {
            position += remaining;
            break;
        }
        if (contacts != nullptr && std::ranges::none_of(*contacts, [&](const WallContact& seen) {
                return seen.object == nearest.contact.object;
            })) {
            contacts->push_back(nearest.contact);
        }
        position += nearest.time * remaining;
        remaining *= 1.0f - nearest.time;
        remaining -= glm::dot(remaining, nearest.normal) * nearest.normal;
    }
    return Vec3{position.x, to.y, position.y};
}

std::vector<WallContact> WorldCollision::surfaceContacts(const Vec3& centre, f32 radius, f32 bottom,
                                                         f32 top) const {
    std::vector<WallContact> contacts;
    if (radius <= 0 || top < bottom) {
        return contacts;
    }
    eachTriangle(
        centre.x - radius, centre.z - radius, centre.x + radius, centre.z + radius,
        [&](const CollisionTriangle& triangle) {
            if ((triangle.objectFlags & (kWallQueryFlags | kFloorQueryFlags)) == 0 ||
                (triangle.objectFlags & kLiquidSurface) != 0) {
                return;
            }
            // Clip to the body's vertical interval, then intersect the resulting
            // convex footprint with its circular cross-section. Two wall slices
            // miss the lower/upper hemispheres of moving lava balls entirely.
            std::array<Vec3, 8> polygon{triangle.vertices[0], triangle.vertices[1],
                                        triangle.vertices[2]};
            usize count = triangle.vertices.size();
            for (const auto& [height, above] : {std::pair{bottom, true}, std::pair{top, false}}) {
                std::array<Vec3, 8> clipped{};
                usize clippedCount = 0;
                for (usize i = 0; i < count; ++i) {
                    const Vec3 a = polygon[i];
                    const Vec3 b = polygon[(i + 1) % count];
                    const bool inA = above ? a.y >= height : a.y <= height;
                    const bool inB = above ? b.y >= height : b.y <= height;
                    if (inA) {
                        clipped[clippedCount++] = a;
                    }
                    if (inA != inB) {
                        clipped[clippedCount++] = glm::mix(a, b, (height - a.y) / (b.y - a.y));
                    }
                }
                polygon = clipped;
                count = clippedCount;
            }
            if (count == 0) {
                return;
            }
            const Vec2 here{centre.x, centre.z};
            f32 nearest = radius * radius;
            Vec3 point{};
            bool touches = false;
            bool positive = false;
            bool negative = false;
            f32 area = 0;
            for (usize i = 0; i < count; ++i) {
                const Vec3 a = polygon[i];
                const Vec3 b = polygon[(i + 1) % count];
                const Vec2 start{a.x, a.z};
                const Vec2 edge{b.x - a.x, b.z - a.z};
                const f32 side = edge.x * (here.y - start.y) - edge.y * (here.x - start.x);
                positive |= side > kSweepEpsilon;
                negative |= side < -kSweepEpsilon;
                area += (a.x - centre.x) * (b.z - centre.z) - (b.x - centre.x) * (a.z - centre.z);
                const f32 square = glm::dot(edge, edge);
                const f32 t =
                    square > 0 ? std::clamp(glm::dot(here - start, edge) / square, 0.0f, 1.0f) : 0;
                const Vec3 candidate = glm::mix(a, b, t);
                const Vec2 delta{candidate.x - centre.x, candidate.z - centre.z};
                const f32 distance = glm::dot(delta, delta);
                if (distance <= nearest) {
                    nearest = distance;
                    point = candidate;
                    touches = true;
                }
            }
            if (std::abs(area) > kSweepEpsilon && !(positive && negative)) {
                touches = true;
                point = {centre.x, std::clamp(centre.y, bottom, top), centre.z};
            }
            if (touches && std::ranges::none_of(contacts, [&](const WallContact& seen) {
                    return seen.object == triangle.object;
                })) {
                contacts.push_back({triangle.object, point});
            }
        });
    return contacts;
}

Vec3 WorldCollision::resolveWalls(const Vec3& centre, f32 radius, f32 bottom, f32 top,
                                  std::vector<WallContact>* contacts, std::optional<f32> minimumY,
                                  WallPush push, u32 mask) const {
    Vec3 out = centre;
    const f32 reach = radius * 2.0f;
    for (s32 pass = 0; pass < kPasses; ++pass) {
        bool pushed = false;
        eachTriangle(
            out.x - reach, out.z - reach, out.x + reach, out.z + reach,
            [&](const CollisionTriangle& triangle) {
                if ((triangle.objectFlags & mask) == 0 ||
                    (triangle.objectFlags & kLiquidSurface) != 0 ||
                    std::abs(triangle.normal.y) >= kFloorNormalY) {
                    return; // a floor or a ceiling
                }
                if (minimumY && std::ranges::all_of(triangle.vertices, [&](const Vec3& point) {
                        return point.y < *minimumY;
                    })) {
                    return;
                }
                Vec2 wallNormal{triangle.normal.x, triangle.normal.z};
                const f32 normalLength = glm::length(wallNormal);
                wallNormal = normalLength > kEpsilon ? wallNormal / normalLength : Vec2{1.0f, 0.0f};
                for (const f32 fraction : kProbeFractions) {
                    // Match the swept contact interval; a later overlap check
                    // must not push a player back off a successfully boarded rail.
                    const f32 height =
                        std::max(bottom + (top - bottom) * fraction, minimumY.value_or(bottom));
                    if (height > top) {
                        continue;
                    }
                    const Slice slice = sliceAt(triangle, height);
                    if (!slice.valid) {
                        continue;
                    }
                    const Vec2 here{out.x, out.z};
                    const Vec2 nearest = closestOnSegment(slice.a, slice.b, here);
                    const Vec2 away = here - nearest;
                    const f32 distance = glm::length(away);
                    if (distance >= radius) {
                        continue;
                    }
                    if (contacts != nullptr &&
                        std::ranges::none_of(*contacts, [&](const WallContact& seen) {
                            return seen.object == triangle.object;
                        })) {
                        contacts->push_back(
                            WallContact{triangle.object, Vec3{nearest.x, height, nearest.y}});
                    }
                    if (contactOnly(triangle.object) ||
                        (push == WallPush::StaticOnly && (triangle.objectFlags & 0x1000U) != 0)) {
                        continue;
                    }
                    // Push straight away from the wall when in front of it, else out along
                    // its normal so a mover never ends up behind it.
                    Vec2 direction = wallNormal;
                    f32 depth = radius - glm::dot(away, wallNormal);
                    if (distance > kEpsilon && glm::dot(away, wallNormal) > 0.0f) {
                        direction = away / distance;
                        depth = radius - distance;
                    }
                    out.x += direction.x * depth;
                    out.z += direction.y * depth;
                    pushed = true;
                }
            });
        if (!pushed) {
            break;
        }
    }
    return out;
}

namespace {

/** The earliest time in [0, 1] a sphere of `radius` moving `from` by `step` touches the
 * point `vertex`; none when it never does within the step. */
std::optional<f32> sphereMeetsPoint(const Vec3& from, const Vec3& step, f32 radius,
                                    const Vec3& vertex) {
    const Vec3 offset = from - vertex;
    const f32 c = glm::dot(offset, offset) - radius * radius;
    if (c <= 0.0f) {
        return 0.0f; // touching already
    }
    const f32 a = glm::dot(step, step);
    const f32 b = 2.0f * glm::dot(offset, step);
    if (a <= kSweepEpsilon || b >= 0.0f) {
        return std::nullopt; // still, or moving away
    }
    const f32 discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f) {
        return std::nullopt;
    }
    const f32 t = (-b - std::sqrt(discriminant)) / (2.0f * a);
    return t >= 0.0f && t <= 1.0f ? std::optional{t} : std::nullopt;
}

/** The earliest time the moving sphere touches the segment `a`-`b` between its ends, and
 * where on the segment. */
std::optional<std::pair<f32, Vec3>> sphereMeetsSegment(const Vec3& from, const Vec3& step,
                                                       f32 radius, const Vec3& a, const Vec3& b) {
    const Vec3 edge = b - a;
    const f32 length = glm::length(edge);
    if (length <= kSweepEpsilon) {
        return std::nullopt;
    }
    const Vec3 along = edge / length;
    const Vec3 w = from - a;
    const Vec3 wSide = w - along * glm::dot(w, along);
    const Vec3 stepSide = step - along * glm::dot(step, along);
    const f32 c = glm::dot(wSide, wSide) - radius * radius;
    f32 t = 0.0f;
    if (c > 0.0f) {
        const f32 aa = glm::dot(stepSide, stepSide);
        const f32 bb = 2.0f * glm::dot(wSide, stepSide);
        if (aa <= kSweepEpsilon || bb >= 0.0f) {
            return std::nullopt;
        }
        const f32 discriminant = bb * bb - 4.0f * aa * c;
        if (discriminant < 0.0f) {
            return std::nullopt;
        }
        t = (-bb - std::sqrt(discriminant)) / (2.0f * aa);
        if (t < 0.0f || t > 1.0f) {
            return std::nullopt;
        }
    }
    const f32 foot = glm::dot(from + step * t - a, along);
    if (foot < 0.0f || foot > length) {
        return std::nullopt; // past an end: the vertex test has it
    }
    return std::pair{t, a + along * foot};
}

/** BTriLineCol for a wall: the first contact of the sphere with the triangle's front, as a
 * time along the step and the point of the face met. */
std::optional<std::pair<f32, Vec3>> sphereMeetsFront(const CollisionTriangle& triangle,
                                                     const Vec3& from, const Vec3& step,
                                                     f32 radius) {
    const Vec3& origin = triangle.vertices[0];
    const Vec3& normal = triangle.normal;
    const f32 start = glm::dot(from - origin, normal);
    const f32 end = glm::dot(from + step - origin, normal);
    if (start < kOnPlane || end > start || (start > radius && end > radius)) {
        return std::nullopt; // behind or on it, parting from it, or never within reach
    }
    std::optional<std::pair<f32, Vec3>> best;
    const auto offer = [&](f32 time, const Vec3& point) {
        if (!best || time < best->first) {
            best = std::pair{time, point};
        }
    };
    if (start <= radius) {
        const Vec3 foot = from - normal * start;
        if (insideFace(triangle, foot)) {
            offer(0.0f, foot);
        }
    } else {
        const f32 time = (start - radius) / (start - end);
        const Vec3 foot = from + step * time - normal * radius;
        if (insideFace(triangle, foot)) {
            offer(time, foot);
        }
    }
    for (usize i = 0; i < triangle.vertices.size(); ++i) {
        const Vec3& a = triangle.vertices[i];
        const Vec3& b = triangle.vertices[(i + 1) % triangle.vertices.size()];
        if (const auto edge = sphereMeetsSegment(from, step, radius, a, b)) {
            offer(edge->first, edge->second);
        }
        if (const auto corner = sphereMeetsPoint(from, step, radius, a)) {
            offer(*corner, a);
        }
    }
    return best;
}

} // namespace

std::optional<SphereHit> WorldCollision::sweepSphere(const Vec3& from, const Vec3& to, f32 radius,
                                                     u32 mask) const {
    radius = std::max(0.0f, radius);
    const Vec3 step = to - from;
    std::optional<SphereHit> best;
    const auto consider = [&](const CollisionTriangle& triangle, f32 localNormalY) {
        if (contactOnly(triangle.object) || (triangle.objectFlags & mask) == 0 ||
            (triangle.objectFlags & kLiquidSurface) != 0) {
            return;
        }
        // CTriListCollide keeps the wall band by the object's own normal.
        if ((triangle.objectFlags & kUnfilteredSurface) == 0 &&
            (localNormalY < -kRayWallNormalY || localNormalY > kRayWallNormalY)) {
            return;
        }
        const auto met = sphereMeetsFront(triangle, from, step, radius);
        if (met && (!best || met->first < best->time)) {
            best = SphereHit{triangle.object, triangle.objectFlags, triangle.normal, met->second,
                             met->first};
        }
    };
    const f32 minX = std::min(from.x, to.x) - radius;
    const f32 minZ = std::min(from.z, to.z) - radius;
    const f32 maxX = std::max(from.x, to.x) + radius;
    const f32 maxZ = std::max(from.z, to.z) + radius;
    for (const u32 index : candidates(minX, minZ, maxX, maxZ)) {
        const CollisionTriangle& triangle = m_triangles[index];
        if (solid(triangle.object)) {
            consider(triangle, triangle.normal.y);
        }
    }
    for (const MovingObject& mover : m_moving) {
        if (!mover.overlaps(minX, minZ, maxX, maxZ) || !solid(mover.object)) {
            continue;
        }
        for (usize i = 0; i < mover.placed.size(); ++i) {
            if (mover.placed[i].normal != Vec3{0}) {
                consider(mover.placed[i], mover.local[i].normal.y);
            }
        }
    }
    return best;
}

std::optional<FloorHit> WorldCollision::floorUnder(const Vec3& origin, f32 depth,
                                                   f32 radius) const {
    radius = std::max(0.0f, radius);
    const f32 lowest = origin.y - depth - radius;
    std::optional<FloorHit> face;
    std::optional<FloorHit> edge;
    f32 edgeDistance = radius * radius;
    eachTriangle(origin.x - radius, origin.z - radius, origin.x + radius, origin.z + radius,
                 [&](const CollisionTriangle& triangle) {
                     if ((triangle.objectFlags & kFloorQueryFlags) == 0 ||
                         (triangle.objectFlags & kLiquidSurface) != 0 ||
                         triangle.normal.y < kFloorNormalY) {
                         return;
                     }
                     const Vec3& v = triangle.vertices[0];
                     const auto heightAt = [&](const Vec2& point) {
                         return v.y - (triangle.normal.x * (point.x - v.x) +
                                       triangle.normal.z * (point.y - v.z)) /
                                          triangle.normal.y;
                     };
                     const Vec2 here{origin.x, origin.z};
                     if (insideXZ(triangle, here.x, here.y)) {
                         const f32 y = heightAt(here);
                         if (y <= origin.y && y >= lowest && (!face || y > face->y)) {
                             face = FloorHit{y,
                                             triangle.normal,
                                             triangle.object,
                                             triangle.objectFlags,
                                             {here.x, y, here.y}};
                         }
                         return;
                     }
                     if (face.has_value() || radius <= 0.0f) {
                         return; // a face under the point outranks every edge
                     }
                     for (usize i = 0; i < triangle.vertices.size(); ++i) {
                         const Vec3& a = triangle.vertices[i];
                         const Vec3& b = triangle.vertices[(i + 1) % triangle.vertices.size()];
                         const Vec2 point = closestOnSegment({a.x, a.z}, {b.x, b.z}, here);
                         const Vec2 delta = point - here;
                         const f32 distance = glm::dot(delta, delta);
                         if (distance > edgeDistance) {
                             continue;
                         }
                         const f32 y = heightAt(point);
                         // The sphere reaches less far down away from its centre line.
                         const f32 reach = std::sqrt(std::max(0.0f, radius * radius - distance));
                         if (y > origin.y || y < origin.y - depth - reach) {
                             continue;
                         }
                         edgeDistance = distance;
                         edge = FloorHit{y,
                                         triangle.normal,
                                         triangle.object,
                                         triangle.objectFlags,
                                         {point.x, y, point.y}};
                     }
                 });
    return face ? face : edge;
}

namespace {

/** The point of the triangle nearest `point`. */
Vec3 closestOnTriangle(const CollisionTriangle& triangle, const Vec3& point) {
    const Vec3& origin = triangle.vertices[0];
    const Vec3 foot = point - triangle.normal * glm::dot(point - origin, triangle.normal);
    if (insideFace(triangle, foot)) {
        return foot;
    }
    Vec3 best = origin;
    f32 bestDistance = std::numeric_limits<f32>::max();
    for (usize i = 0; i < triangle.vertices.size(); ++i) {
        const Vec3& a = triangle.vertices[i];
        const Vec3& b = triangle.vertices[(i + 1) % triangle.vertices.size()];
        const Vec3 edge = b - a;
        const f32 length = glm::dot(edge, edge);
        const f32 t = length > kSweepEpsilon
                          ? std::clamp(glm::dot(point - a, edge) / length, 0.0f, 1.0f)
                          : 0.0f;
        const Vec3 candidate = a + edge * t;
        const f32 distance = glm::dot(candidate - point, candidate - point);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

} // namespace

Vec3 WorldCollision::resolveSphere(const Vec3& centre, f32 radius, bool staticOnly,
                                   u32 mask) const {
    radius = std::max(0.0f, radius);
    Vec3 out = centre;
    for (s32 pass = 0; pass < kPasses; ++pass) {
        bool pushed = false;
        const auto consider = [&](const CollisionTriangle& triangle, f32 localNormalY) {
            if (contactOnly(triangle.object) || (triangle.objectFlags & mask) == 0 ||
                (triangle.objectFlags & kLiquidSurface) != 0) {
                return;
            }
            if ((triangle.objectFlags & kUnfilteredSurface) == 0 &&
                (localNormalY < -kRayWallNormalY || localNormalY > kRayWallNormalY)) {
                return;
            }
            const f32 start = glm::dot(out - triangle.vertices[0], triangle.normal);
            if (start < kOnPlane || start >= radius) {
                return; // behind or on the face, or clear of its plane
            }
            const Vec3 nearest = closestOnTriangle(triangle, out);
            if (glm::dot(nearest - out, nearest - out) >= radius * radius) {
                return;
            }
            out += triangle.normal * (radius - start);
            pushed = true;
        };
        const f32 minX = out.x - radius;
        const f32 minZ = out.z - radius;
        const f32 maxX = out.x + radius;
        const f32 maxZ = out.z + radius;
        for (const u32 index : candidates(minX, minZ, maxX, maxZ)) {
            const CollisionTriangle& triangle = m_triangles[index];
            if (solid(triangle.object)) {
                consider(triangle, triangle.normal.y);
            }
        }
        if (!staticOnly) {
            for (const MovingObject& mover : m_moving) {
                if (!mover.overlaps(minX, minZ, maxX, maxZ) || !solid(mover.object)) {
                    continue;
                }
                for (usize i = 0; i < mover.placed.size(); ++i) {
                    if (mover.placed[i].normal != Vec3{0}) {
                        consider(mover.placed[i], mover.local[i].normal.y);
                    }
                }
            }
        }
        if (!pushed) {
            break;
        }
    }
    return out;
}

bool WorldCollision::wallBetween(const Vec3& from, const Vec3& to) const {
    return wallBetween(from, to, 0);
}

bool WorldCollision::wallBetween(const Vec3& from, const Vec3& to, f32 radius) const {
    radius = std::max(0.0f, radius);
    const auto blocks = [&](const CollisionTriangle& triangle, f32 normalY) {
        if (contactOnly(triangle.object) || (triangle.objectFlags & kWallQueryFlags) == 0 ||
            (triangle.objectFlags & kLiquidSurface) != 0) {
            return false;
        }
        if ((triangle.objectFlags & kUnfilteredSurface) == 0 &&
            (normalY < -kRayWallNormalY || normalY > kRayWallNormalY)) {
            return false;
        }
        return radius > 0 ? sweepsFront(triangle, from, to, radius)
                          : crossesFront(triangle, from, to);
    };
    const f32 minX = std::min(from.x, to.x) - radius;
    const f32 minZ = std::min(from.z, to.z) - radius;
    const f32 maxX = std::max(from.x, to.x) + radius;
    const f32 maxZ = std::max(from.z, to.z) + radius;
    for (const u32 index : candidates(minX, minZ, maxX, maxZ)) {
        const CollisionTriangle& triangle = m_triangles[index];
        if (solid(triangle.object) && blocks(triangle, triangle.normal.y)) {
            return true;
        }
    }
    for (const MovingObject& mover : m_moving) {
        if (!mover.overlaps(minX, minZ, maxX, maxZ) || !solid(mover.object)) {
            continue;
        }
        for (usize i = 0; i < mover.placed.size(); ++i) {
            // WorldObjCollide transforms the ray into the object's space before
            // CTriListCollide filters normals. Rotation must not change that filter.
            if (mover.placed[i].normal != Vec3{0} &&
                blocks(mover.placed[i], mover.local[i].normal.y)) {
                return true;
            }
        }
    }
    return false;
}

std::optional<Vec3> WorldCollision::pickSurface(const Vec3& from, const Vec3& to) const {
    const Vec3 direction = to - from;
    f32 nearest = 1.0f;
    std::optional<Vec3> hit;
    eachTriangle(std::min(from.x, to.x), std::min(from.z, to.z), std::max(from.x, to.x),
                 std::max(from.z, to.z), [&](const CollisionTriangle& triangle) {
                     if (contactOnly(triangle.object) ||
                         (triangle.objectFlags & (kFloorQueryFlags | kWallQueryFlags)) == 0) {
                         return;
                     }
                     const Vec3 edge1 = triangle.vertices[1] - triangle.vertices[0];
                     const Vec3 edge2 = triangle.vertices[2] - triangle.vertices[0];
                     const Vec3 p = glm::cross(direction, edge2);
                     const f32 determinant = glm::dot(edge1, p);
                     if (std::abs(determinant) < 1.0e-6f) {
                         return;
                     }
                     const Vec3 offset = from - triangle.vertices[0];
                     const f32 u = glm::dot(offset, p) / determinant;
                     const Vec3 q = glm::cross(offset, edge1);
                     const f32 v = glm::dot(direction, q) / determinant;
                     const f32 t = glm::dot(edge2, q) / determinant;
                     if (u >= 0 && v >= 0 && u + v <= 1 && t >= 0 && t <= nearest) {
                         nearest = t;
                         hit = from + direction * t;
                     }
                 });
    return hit;
}
} // namespace gdl
