#include "engine/world/WorldCollision.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

using Json = nlohmann::json;

constexpr f32 kEpsilon = 0.001f;
constexpr s32 kPasses = 4;
/** Heights within a cylinder at which walls are checked: about the knees and the chest. */
constexpr std::array<f32, 2> kProbeFractions{0.25f, 0.75f};

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

Vec3 readVec3(const Json& array, usize first) {
    return Vec3{array.at(first).get<f32>(), array.at(first + 1).get<f32>(),
                array.at(first + 2).get<f32>()};
}

} // namespace

bool WorldCollision::load(const std::filesystem::path& directory, const WorldLayout& layout) {
    clear();
    const std::filesystem::path file = directory / "collision.json";
    try {
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
        build(std::move(triangles));
    } catch (const std::exception& e) {
        log::warn("World collision: cannot read {}: {}", file.string(), e.what());
        clear();
        return false;
    }
    return loaded();
}

void WorldCollision::build(std::vector<CollisionTriangle> triangles) {
    clear();
    m_triangles = std::move(triangles);
    index();
}

void WorldCollision::append(std::span<const CollisionTriangle> triangles) {
    m_triangles.insert(m_triangles.end(), triangles.begin(), triangles.end());
    index();
}

void WorldCollision::MovingObject::place(const Mat4& world) {
    this->world = world;
    const Mat3 rotation{world};
    bool first = true;
    for (usize i = 0; i < local.size(); ++i) {
        const CollisionTriangle& from = local[i];
        CollisionTriangle& to = placed[i];
        to.normal = glm::normalize(rotation * from.normal);
        for (usize k = 0; k < 3; ++k) {
            to.vertices[k] = Vec3{world * Vec4{from.vertices[k], 1.0f}};
            boundsMin = first ? to.vertices[k] : glm::min(boundsMin, to.vertices[k]);
            boundsMax = first ? to.vertices[k] : glm::max(boundsMax, to.vertices[k]);
            first = false;
        }
    }
}

void WorldCollision::setMovingObjects(std::span<const s32> objects) {
    for (const s32 object : objects) {
        if (moving(object)) {
            continue;
        }
        MovingObject mover;
        mover.object = object;
        for (const CollisionTriangle& triangle : m_triangles) {
            if (triangle.object == object) {
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
            mover.place(world);
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
    return object >= 0 && solid(object) &&
           std::ranges::find(m_blockedFloorExits, object) != m_blockedFloorExits.end();
}

bool WorldCollision::solid(s32 object) const {
    return std::ranges::find(m_hidden, object) == m_hidden.end();
}

bool WorldCollision::moving(s32 object) const {
    return std::ranges::any_of(m_moving,
                               [&](const MovingObject& mover) { return mover.object == object; });
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
void WorldCollision::eachTriangle(f32 minX, f32 minZ, f32 maxX, f32 maxZ,
                                  const Visit& visit) const {
    for (const u32 index : candidates(minX, minZ, maxX, maxZ)) {
        const CollisionTriangle& triangle = m_triangles[index];
        if (solid(triangle.object)) {
            visit(triangle);
        }
    }
    for (const MovingObject& mover : m_moving) {
        if (!mover.overlaps(minX, minZ, maxX, maxZ) || !solid(mover.object)) {
            continue;
        }
        for (const CollisionTriangle& triangle : mover.placed) {
            visit(triangle);
        }
    }
}

void WorldCollision::clear() {
    m_triangles.clear();
    m_moving.clear();
    m_hidden.clear();
    m_blockedFloorExits.clear();
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
                                                f32 edgeReach) const {
    return surfaceAt(position, above, below, false, std::max(0.0f, edgeReach));
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
                                                  bool liquid, f32 edgeReach,
                                                  const Vec2* direction) const {
    std::optional<FloorHit> best;
    const f32 highest = position.y + above;
    const f32 lowest = position.y - below;
    eachTriangle(position.x - edgeReach, position.z - edgeReach, position.x + edgeReach,
                 position.z + edgeReach, [&](const CollisionTriangle& triangle) {
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
                     const f32 y = v.y - (triangle.normal.x * (point.x - v.x) +
                                          triangle.normal.z * (point.y - v.z)) /
                                             triangle.normal.y;
                     if (y > highest || y < lowest) {
                         return;
                     }
                     if (!best.has_value() || y > best->y) {
                         best = FloorHit{y, triangle.normal, triangle.object, triangle.objectFlags};
                     }
                 });
    return best;
}

Vec3 WorldCollision::resolveWalls(const Vec3& centre, f32 radius, f32 bottom, f32 top,
                                  std::vector<WallContact>* contacts) const {
    Vec3 out = centre;
    const f32 reach = radius * 2.0f;
    for (s32 pass = 0; pass < kPasses; ++pass) {
        bool pushed = false;
        eachTriangle(out.x - reach, out.z - reach, out.x + reach, out.z + reach,
                     [&](const CollisionTriangle& triangle) {
                         if ((triangle.objectFlags & kWallQueryFlags) == 0 ||
                             (triangle.objectFlags & kLiquidSurface) != 0 ||
                             std::abs(triangle.normal.y) >= kFloorNormalY) {
                             return; // a floor or a ceiling
                         }
                         Vec2 wallNormal{triangle.normal.x, triangle.normal.z};
                         const f32 normalLength = glm::length(wallNormal);
                         wallNormal =
                             normalLength > kEpsilon ? wallNormal / normalLength : Vec2{1.0f, 0.0f};
                         for (const f32 fraction : kProbeFractions) {
                             const f32 height = bottom + (top - bottom) * fraction;
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
                                 contacts->push_back(WallContact{
                                     triangle.object, Vec3{nearest.x, height, nearest.y}});
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

} // namespace gdl
