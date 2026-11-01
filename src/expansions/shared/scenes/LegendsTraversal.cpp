#include "expansions/shared/scenes/LegendsTraversal.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Error.h"

#include "game/players/CursorAim.h"

namespace gdl::game {
namespace {
constexpr f32 kStartProbe = 12.0f;
constexpr f32 kPositionLimit = 100'000.0f;
constexpr f32 kNormalEpsilon = 0.0001f;
constexpr f32 kLostMargin = 10.0f;
constexpr usize kMaxTriangles = 1'000'000;
constexpr u64 kMaxGridCells = 1'000'000;
constexpr u64 kMaxCellReferences = 8'000'000;

bool validPosition(const Vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z) &&
           glm::all(glm::lessThanEqual(glm::abs(point), Vec3{kPositionLimit}));
}
} // namespace

void LegendsTraversal::open(const formats::LegendsMap& map, s32 player, const CharacterSave& save,
                            const ClassStats* stats) {
    if (player < 0 || player >= 4 || save.character < 0 || save.character >= kClassCount) {
        throw FormatError("Legends traversal: invalid character");
    }
    std::vector<CollisionTriangle> triangles;
    if (map.collision.size() > kMaxTriangles) {
        throw FormatError("Legends traversal: collision budget exceeded");
    }
    triangles.reserve(map.collision.size());
    // Only the N64 reader supplies this explicit authored-draw provenance.
    // e0040958/e003faf4 select walls by native channel220; e003f58c does not
    // impose GameCube's +/- .866 normal band. Keep steep authored wall faces
    // (including Dagger Peak's statue underside) in the imported wall query.
    const bool n64 = std::ranges::any_of(
        map.objects, [](const auto& object) { return object.hasNativeDrawState; });
    Vec3 low{kPositionLimit};
    Vec3 high{-kPositionLimit};
    for (const auto& source : map.collision) {
        if (!validPosition(source.normal) || glm::length(source.normal) < kNormalEpsilon ||
            source.owner < -1) {
            throw FormatError("Legends traversal: invalid collision normal or owner");
        }
        for (const auto& vertex : source.vertices) {
            if (!validPosition(vertex)) {
                throw FormatError("Legends traversal: invalid collision vertex");
            }
        }
        if (!source.floor && !source.wall) {
            continue;
        }
        const Vec3 normal = glm::normalize(source.normal);
        for (const auto& vertex : source.vertices) {
            low = glm::min(low, vertex);
            high = glm::max(high, vertex);
        }
        // These are the port's query channels, not an interpretation of native flags.
        u32 queries = (source.floor ? 4U : 0U) | (source.wall ? 2U : 0U);
        if (n64 && source.wall) {
            // Native projectile collision (e0077738 -> e0040238) requests
            // CTRI channels 0x30, not the body's 0x220. Channel 0x200 bounds
            // ravines without stopping shots. Translate it to the engine's
            // body-only fence channel instead of an ordinary solid wall.
            if ((source.flags & 0x220U) == 0x200U) {
                queries = (queries & ~2U) | 0x100U;
            }
            queries |= 0x40U; // common unfiltered-surface semantic, not copied native flags
        }
        if (source.floor && normal.y >= WorldCollision::kFloorNormalY && normal.y < 0.999f) {
            queries |= PlayerActor::kSlopedFloor;
        }
        triangles.push_back({normal, source.vertices, source.owner, queries});
    }
    if (triangles.empty()) {
        throw FormatError("Legends traversal: no decoded native collision");
    }
    const auto columns =
        static_cast<u64>(std::ceil((high.x - low.x) / WorldCollision::kCellSize)) + 1;
    const auto rows = static_cast<u64>(std::ceil((high.z - low.z) / WorldCollision::kCellSize)) + 1;
    if (columns * rows > kMaxGridCells) {
        throw FormatError("Legends traversal: collision grid exceeds budget");
    }
    u64 references = 0;
    for (const auto& triangle : triangles) {
        const auto& v = triangle.vertices;
        const Vec3 minimum = glm::min(glm::min(v[0], v[1]), v[2]);
        const Vec3 maximum = glm::max(glm::max(v[0], v[1]), v[2]);
        // Indexing includes both end cells. Two extra cells conservatively cover
        // the grid origin's fractional offset without duplicating index internals.
        const u64 width =
            static_cast<u64>(std::ceil((maximum.x - minimum.x) / WorldCollision::kCellSize)) + 2;
        const u64 depth =
            static_cast<u64>(std::ceil((maximum.z - minimum.z) / WorldCollision::kCellSize)) + 2;
        references += width * depth;
        if (references > kMaxCellReferences) {
            throw FormatError("Legends traversal: collision index references exceed budget");
        }
    }
    WorldCollision collision;
    collision.build(std::move(triangles));
    PlayerActor actor;
    actor.spawn(player, save, stats, Vec3{0}, 0);
    const auto standingPoint = [&](const Vec3& requested) -> std::optional<Vec3> {
        const auto floor = collision.floorAt(requested, PlayerActor::kStepUp, kStartProbe);
        if (!floor) {
            return {};
        }
        const Vec3 feet{requested.x, floor->y, requested.z};
        const Vec3 centre = feet + (actor.followPoint() - actor.position()) +
                            Vec3{0, PlayerActor::kWallSphereLift, 0};
        if (glm::distance(collision.resolveSphere(centre, actor.reach()), centre) > 0.01f) {
            return {};
        }
        return feet;
    };
    std::optional<Vec3> start;
    f32 facing = 0;
    bool authored = false;
    for (const auto& marker : map.starts) {
        if (!validPosition(marker.position) || !std::isfinite(marker.facingRadians)) {
            throw FormatError("Legends traversal: invalid start marker");
        }
        if (const auto point = standingPoint(marker.position)) {
            start = point;
            facing = marker.facingRadians;
            authored = true;
            break;
        }
    }
    if (!start) {
        // Some native maps only receive returning players. Until those destination rules
        // are implemented, an explicit exploration fallback uses a real collision face.
        for (const auto& triangle : map.collision) {
            if (!triangle.floor || triangle.normal.y < WorldCollision::kFloorNormalY) {
                continue;
            }
            const Vec3 centre =
                (triangle.vertices[0] + triangle.vertices[1] + triangle.vertices[2]) / 3.0f;
            if (const auto point = standingPoint(centre)) {
                start = point;
                break;
            }
        }
    }
    if (!start) {
        throw FormatError("Legends traversal: no safe floor at an entrance or collision face");
    }
    actor.spawn(player, save, stats, *start, facing);
    actor.settle(collision);
    m_collision = std::move(collision);
    m_actor = std::move(actor);
    m_start = *start;
    m_authoredStart = authored;
}

void LegendsTraversal::step(const MoveInput& input, f32 cameraYaw, f32 seconds,
                            const std::optional<Vec3>& aim, bool freeMove) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 0.1f || !std::isfinite(cameraYaw)) {
        return;
    }
    MoveInput move = input;
    const bool aiming = aim && validPosition(*aim) && glm::length(*aim) > kNormalEpsilon;
    if (aiming) {
        const Vec3 target = m_actor.position() + *aim;
        m_actor.faceToward(target);
        if (!freeMove) {
            move = cursorRelativeMove(move, m_actor.position(), target, cameraYaw);
        }
    }
    m_actor.clearWallContacts();
    const PlayerActor before = m_actor;
    m_actor.update(move, cameraYaw, seconds, &m_collision, 1.0f, aiming);
    const Vec2 displacement{m_actor.position().x - before.position().x,
                            m_actor.position().z - before.position().z};
    const Vec2 requested{m_actor.stepAim().x - before.position().x,
                         m_actor.stepAim().z - before.position().z};
    // The shared GameCube slide response uses a wall's plane normal even at a
    // finite triangle edge. On imported low walls this can push a sphere farther
    // backwards than its intended step. Exploration has no external pushes:
    // conservatively refuse that step instead of oscillating across the edge.
    // Keep this adapter guard separate from the retail movement implementation.
    constexpr f32 kMargin = 0.001f;
    if (glm::length(displacement) > glm::length(requested) + kMargin ||
        glm::dot(displacement, requested) < -kMargin * glm::length(requested)) {
        const f32 facing = m_actor.yaw();
        m_actor = before;
        m_actor.turnTo(facing);
        m_actor.update({}, cameraYaw, seconds, &m_collision, 1.0f, true);
    }
    m_actor.fall(seconds, m_collision);
    if (!validPosition(m_actor.position()) ||
        m_actor.position().y < m_collision.lowest() - kLostMargin) {
        m_actor.place(m_start);
        m_actor.settle(m_collision);
    }
}

bool LegendsTraversal::correctSceneryStep(const PlayerActor& before, const Vec3& correctedPosition,
                                          f32 seconds) {
    return correctImportedSceneryStep(m_actor, before, m_collision, correctedPosition, seconds);
}

bool correctImportedSceneryStep(PlayerActor& actor, const PlayerActor& before,
                                const WorldCollision& collision, const Vec3& correctedPosition,
                                f32 seconds) {
    constexpr f32 kMargin = 0.001f;
    const f32 facing = actor.yaw();
    const auto refuse = [&] {
        CharacterSave save = std::move(actor.save());
        actor = before;
        actor.save() = std::move(save);
        if (std::isfinite(facing)) {
            actor.turnTo(facing);
        }
        return false;
    };
    if (!validPosition(correctedPosition) || !std::isfinite(seconds) || seconds <= 0 ||
        seconds > 0.1f || !std::isfinite(facing) ||
        std::abs(correctedPosition.y - actor.position().y) > kMargin) {
        return refuse();
    }
    const Vec2 offset{correctedPosition.x - before.position().x,
                      correctedPosition.z - before.position().z};
    const Vec2 original{actor.position().x - before.position().x,
                        actor.position().z - before.position().z};
    const f32 distance = glm::length(offset);
    if (distance > glm::length(original) + kMargin) {
        return refuse();
    }
    const Vec3 centre = before.followPoint() + Vec3{0, PlayerActor::kWallSphereLift, 0};
    const Vec3 horizontal{offset.x, 0, offset.y};
    // A second slide along a static wall could put the body back inside the
    // door. Refuse instead of composing two competing collision responses.
    if (collision.sweepSphere(centre, centre + horizontal, before.reach())) {
        return refuse();
    }
    PlayerActor candidate = before;
    candidate.save() = actor.save();
    candidate.clearWallContacts();
    candidate.turnTo(facing);
    // Reuse PlayerActor's complete floor/step/ledge rules, including its rise
    // scaling and bounded descent. update() also records the corrected moving
    // state without retaining the previous frame's animation intent.
    if (distance > 0) {
        const f32 pace = candidate.speed();
        if (!std::isfinite(pace) || pace <= 0) {
            return refuse();
        }
        const f32 scale = distance / (pace * seconds);
        if (!std::isfinite(scale)) {
            return refuse();
        }
        candidate.update({offset / distance, 1}, 0, seconds, &collision, scale, true);
    } else {
        candidate.update({}, 0, seconds, &collision, 1, true);
    }
    const Vec2 reached{candidate.position().x, candidate.position().z};
    if (glm::distance(reached, Vec2{correctedPosition.x, correctedPosition.z}) > kMargin) {
        return refuse();
    }
    candidate.fall(seconds, collision);
    const Vec3 endCentre = candidate.followPoint() + Vec3{0, PlayerActor::kWallSphereLift, 0};
    if (!validPosition(candidate.position()) ||
        collision.sweepSphere(centre, endCentre, candidate.reach()) ||
        glm::distance(collision.resolveSphere(endCentre, candidate.reach()), endCentre) > kMargin) {
        return refuse();
    }
    actor = std::move(candidate);
    return true;
}

} // namespace gdl::game
