#include "engine/world/WorldCollision.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

using Json = nlohmann::json;

constexpr float kEpsilon = 0.001f;
constexpr int kPasses = 4;
/** Heights within a cylinder at which walls are checked: about the knees and the chest. */
constexpr std::array<float, 2> kProbeFractions{0.25f, 0.75f};

/** Whether (x, z) lies inside the triangle's ground-plane projection. */
bool insideXZ(const CollisionTriangle& triangle, float x, float z) {
    const auto side = [&](const Vec3& a, const Vec3& b) {
        return (b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x);
    };
    const float s0 = side(triangle.vertices[0], triangle.vertices[1]);
    const float s1 = side(triangle.vertices[1], triangle.vertices[2]);
    const float s2 = side(triangle.vertices[2], triangle.vertices[0]);
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

Slice sliceAt(const CollisionTriangle& triangle, float y) {
    std::array<Vec2, 6> points{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < 3; ++i) {
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
        const float t = (y - p.y) / (q.y - p.y);
        points[count++] = Vec2{p.x + (q.x - p.x) * t, p.z + (q.z - p.z) * t};
    }
    Slice slice;
    float longest = kEpsilon * kEpsilon;
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            const float length = glm::dot(points[j] - points[i], points[j] - points[i]);
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
    const float length = glm::dot(edge, edge);
    if (length <= kEpsilon * kEpsilon) {
        return a;
    }
    const float t = std::clamp(glm::dot(point - a, edge) / length, 0.0f, 1.0f);
    return a + edge * t;
}

Vec3 readVec3(const Json& array, std::size_t first) {
    return Vec3{array.at(first).get<float>(), array.at(first + 1).get<float>(),
                array.at(first + 2).get<float>()};
}

} // namespace

bool WorldCollision::load(const std::filesystem::path& directory, const WorldLayout& layout) {
    clear();
    const std::filesystem::path file = directory / "collision.json";
    try {
        const Json root = Json::parse(readTextFile(file), nullptr, true, true);
        std::vector<CollisionTriangle> triangles;
        for (const Json& entry : root.at("objects")) {
            const auto object = entry.at("object").get<int>();
            if (object < 0 || static_cast<std::size_t>(object) >= layout.objects().size()) {
                throw FormatError("object index out of range");
            }
            if (layout.objects()[static_cast<std::size_t>(object)].noCollision) {
                continue;
            }
            const Json& normals = entry.at("normals");
            const Json& vertices = entry.at("vertices");
            if (!normals.is_array() || !vertices.is_array() || normals.size() % 3 != 0 ||
                vertices.size() != normals.size() * 3) {
                throw FormatError("malformed triangle lists");
            }
            const std::size_t count = normals.size() / 3;
            for (std::size_t t = 0; t < count; ++t) {
                CollisionTriangle triangle;
                triangle.object = object;
                triangle.normal = readVec3(normals, t * 3);
                for (std::size_t k = 0; k < 3; ++k) {
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

void WorldCollision::MovingObject::place(const Mat4& world) {
    const Mat3 rotation{world};
    bool first = true;
    for (std::size_t i = 0; i < local.size(); ++i) {
        const CollisionTriangle& from = local[i];
        CollisionTriangle& to = placed[i];
        to.normal = glm::normalize(rotation * from.normal);
        for (std::size_t k = 0; k < 3; ++k) {
            to.vertices[k] = Vec3{world * Vec4{from.vertices[k], 1.0f}};
            boundsMin = first ? to.vertices[k] : glm::min(boundsMin, to.vertices[k]);
            boundsMax = first ? to.vertices[k] : glm::max(boundsMax, to.vertices[k]);
            first = false;
        }
    }
}

void WorldCollision::setMovingObjects(std::span<const int> objects) {
    for (const int object : objects) {
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

void WorldCollision::setObjectTransform(int object, const Mat4& world) {
    for (MovingObject& mover : m_moving) {
        if (mover.object == object) {
            mover.place(world);
            return;
        }
    }
}

void WorldCollision::setSolid(int object, bool solid) {
    const auto found = std::ranges::find(m_hidden, object);
    if (solid && found != m_hidden.end()) {
        m_hidden.erase(found);
    } else if (!solid && found == m_hidden.end()) {
        m_hidden.push_back(object);
    }
}

bool WorldCollision::solid(int object) const {
    return std::ranges::find(m_hidden, object) == m_hidden.end();
}

bool WorldCollision::moving(int object) const {
    return std::ranges::any_of(m_moving,
                               [&](const MovingObject& mover) { return mover.object == object; });
}

std::size_t WorldCollision::triangleCount() const {
    std::size_t count = m_triangles.size();
    for (const MovingObject& mover : m_moving) {
        count += mover.local.size();
    }
    return count;
}

template <typename Visit>
void WorldCollision::eachTriangle(float minX, float minZ, float maxX, float maxZ,
                                  const Visit& visit) const {
    for (const unsigned int index : candidates(minX, minZ, maxX, maxZ)) {
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
    m_columns = static_cast<unsigned int>(std::ceil((m_max.x - m_min.x) / kCellSize)) + 1;
    m_rows = static_cast<unsigned int>(std::ceil((m_max.z - m_min.z) / kCellSize)) + 1;
    m_cells.assign(std::size_t{m_columns} * m_rows, {});
    const auto column = [&](float x) {
        return static_cast<unsigned int>(
            std::clamp((x - m_min.x) / kCellSize, 0.0f, static_cast<float>(m_columns - 1)));
    };
    const auto row = [&](float z) {
        return static_cast<unsigned int>(
            std::clamp((z - m_min.z) / kCellSize, 0.0f, static_cast<float>(m_rows - 1)));
    };
    for (std::size_t i = 0; i < m_triangles.size(); ++i) {
        const auto& v = m_triangles[i].vertices;
        const float minX = std::min({v[0].x, v[1].x, v[2].x});
        const float maxX = std::max({v[0].x, v[1].x, v[2].x});
        const float minZ = std::min({v[0].z, v[1].z, v[2].z});
        const float maxZ = std::max({v[0].z, v[1].z, v[2].z});
        for (unsigned int r = row(minZ); r <= row(maxZ); ++r) {
            for (unsigned int c = column(minX); c <= column(maxX); ++c) {
                m_cells[std::size_t{r} * m_columns + c].push_back(static_cast<unsigned int>(i));
            }
        }
    }
}

std::vector<unsigned int> WorldCollision::candidates(float minX, float minZ, float maxX,
                                                     float maxZ) const {
    std::vector<unsigned int> out;
    if (m_cells.empty()) {
        return out;
    }
    const auto column = [&](float x) {
        return static_cast<unsigned int>(
            std::clamp((x - m_min.x) / kCellSize, 0.0f, static_cast<float>(m_columns - 1)));
    };
    const auto row = [&](float z) {
        return static_cast<unsigned int>(
            std::clamp((z - m_min.z) / kCellSize, 0.0f, static_cast<float>(m_rows - 1)));
    };
    for (unsigned int r = row(minZ); r <= row(maxZ); ++r) {
        for (unsigned int c = column(minX); c <= column(maxX); ++c) {
            const std::vector<unsigned int>& cell = m_cells[std::size_t{r} * m_columns + c];
            out.insert(out.end(), cell.begin(), cell.end());
        }
    }
    std::ranges::sort(out);
    const auto duplicates = std::ranges::unique(out);
    out.erase(duplicates.begin(), duplicates.end());
    return out;
}

std::optional<FloorHit> WorldCollision::floorAt(const Vec3& position, float above,
                                                float below) const {
    std::optional<FloorHit> best;
    const float highest = position.y + above;
    const float lowest = position.y - below;
    eachTriangle(
        position.x, position.z, position.x, position.z, [&](const CollisionTriangle& triangle) {
            if (triangle.normal.y < kFloorNormalY || !insideXZ(triangle, position.x, position.z)) {
                return;
            }
            const Vec3& v = triangle.vertices[0];
            const float y = v.y - (triangle.normal.x * (position.x - v.x) +
                                   triangle.normal.z * (position.z - v.z)) /
                                      triangle.normal.y;
            if (y > highest || y < lowest) {
                return;
            }
            if (!best.has_value() || y > best->y) {
                best = FloorHit{y, triangle.normal, triangle.object};
            }
        });
    return best;
}

Vec3 WorldCollision::resolveWalls(const Vec3& centre, float radius, float bottom, float top) const {
    Vec3 out = centre;
    const float reach = radius * 2.0f;
    for (int pass = 0; pass < kPasses; ++pass) {
        bool pushed = false;
        eachTriangle(out.x - reach, out.z - reach, out.x + reach, out.z + reach,
                     [&](const CollisionTriangle& triangle) {
                         if (std::abs(triangle.normal.y) >= kFloorNormalY) {
                             return; // a floor or a ceiling
                         }
                         Vec2 wallNormal{triangle.normal.x, triangle.normal.z};
                         const float normalLength = glm::length(wallNormal);
                         wallNormal =
                             normalLength > kEpsilon ? wallNormal / normalLength : Vec2{1.0f, 0.0f};
                         for (const float fraction : kProbeFractions) {
                             const Slice slice =
                                 sliceAt(triangle, bottom + (top - bottom) * fraction);
                             if (!slice.valid) {
                                 continue;
                             }
                             const Vec2 here{out.x, out.z};
                             const Vec2 away = here - closestOnSegment(slice.a, slice.b, here);
                             const float distance = glm::length(away);
                             if (distance >= radius) {
                                 continue;
                             }
                             // Push straight away from the wall when in front of it, else out along
                             // its normal so a mover never ends up behind it.
                             Vec2 direction = wallNormal;
                             float depth = radius - glm::dot(away, wallNormal);
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
