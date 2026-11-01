#include "expansions/n64/level/LegendsTowerMotion.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gdl::game {
namespace {

Vec3 resolveCylinder(const Vec3& centre, f32 cylinderRadius, f32 halfHeight, const Vec3& from,
                     const Vec3& to, f32 radius, f32 height) {
    const auto finite = [](const Vec3& point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    };
    if (!finite(from)) {
        throw std::invalid_argument("Tower door movement requires a finite starting position");
    }
    if (!finite(to) || !finite(centre) || !std::isfinite(radius) || radius < 0 ||
        !std::isfinite(height) || height < 0 || !std::isfinite(cylinderRadius) ||
        cylinderRadius <= 0 || !std::isfinite(halfHeight) || halfHeight <= 0) {
        return from;
    }
    // Tower.bin e0864e7c creates shape0/radius9; game.bin e0047ae4 creates
    // player shape1. Their e0060280 contact test is XZ radius sum plus overlap
    // between sphere Y+-radius and the player's feetY..feetY+height. Do not use
    // the doorway radius as the player's height or as a 3D distance cutoff.
    // The response below deliberately uses continuous GDL movement, not N64's
    // 1/32-unit endpoint solver (e0048158 case0xf, e00486f8 two retries).
    const f64 x = static_cast<f64>(from.x) - centre.x;
    const f64 z = static_cast<f64>(from.z) - centre.z;
    const f64 dx = static_cast<f64>(to.x) - from.x;
    const f64 dz = static_cast<f64>(to.z) - from.z;
    const f64 dy = static_cast<f64>(to.y) - from.y;
    const f64 combined = static_cast<f64>(cylinderRadius) + radius;
    const f64 distanceSquared = x * x + z * z;
    const f64 radiusSquared = combined * combined;
    const f64 speedSquared = dx * dx + dz * dz;
    f64 enter = 0;
    f64 leave = 1;
    if (speedSquared == 0) {
        if (distanceSquared >= radiusSquared) {
            return to;
        }
    } else {
        const f64 dot = x * dx + z * dz;
        const f64 discriminant = dot * dot - speedSquared * (distanceSquared - radiusSquared);
        if (discriminant <= 0) {
            return to;
        }
        const f64 root = std::sqrt(discriminant);
        enter = std::max(enter, (-dot - root) / speedSquared);
        leave = std::min(leave, (-dot + root) / speedSquared);
    }
    const f64 bottom = static_cast<f64>(centre.y) - halfHeight - height;
    const f64 top = static_cast<f64>(centre.y) + halfHeight;
    if (dy == 0) {
        if (from.y < bottom || from.y > top) {
            return to;
        }
    } else {
        const f64 a = (bottom - from.y) / dy;
        const f64 b = (top - from.y) / dy;
        enter = std::max(enter, std::min(a, b));
        leave = std::min(leave, std::max(a, b));
    }
    if (enter >= leave || enter > 1 || leave < 0) {
        return to;
    }
    if (distanceSquared < radiusSquared && from.y >= bottom && from.y <= top) {
        // A pre-existing overlap must not become a trap. Permit monotonic
        // escape, but not crossing through the centre to the opposite side.
        const f64 radial = x * dx + z * dz;
        const bool horizontalEscape = speedSquared > 0 && radial >= 0;
        const f64 bodyCentre = static_cast<f64>(from.y) + height * .5;
        const bool verticalEscape =
            dy != 0 && (bodyCentre - centre.y) * dy >= 0 && speedSquared == 0;
        return horizontalEscape || verticalEscape ? to : from;
    }
    const f64 hitX = x + dx * enter;
    const f64 hitZ = z + dz * enter;
    const f64 hitLength = std::sqrt(hitX * hitX + hitZ * hitZ);
    // Entering from above/below while horizontally inside cannot be corrected
    // with a tangent without jumping the player sideways; refuse instead.
    if (hitLength < combined - 1e-6 || hitLength == 0) {
        return from;
    }
    const f64 nx = hitX / hitLength;
    const f64 nz = hitZ / hitLength;
    f64 remainingX = dx * (1 - enter);
    f64 remainingZ = dz * (1 - enter);
    const f64 inward = std::min(0.0, remainingX * nx + remainingZ * nz);
    remainingX -= inward * nx;
    remainingZ -= inward * nz;
    const Vec3 result{static_cast<f32>(from.x + dx * enter + remainingX), to.y,
                      static_cast<f32>(from.z + dz * enter + remainingZ)};
    if (!finite(result)) {
        return from;
    }
    return result;
}

} // namespace

Vec3 resolveLegendsTowerDoor(const formats::LegendsN64TowerDoor& door, const Vec3& from,
                             const Vec3& to, f32 radius, f32 height) {
    return resolveCylinder(door.triggerPosition, door.triggerRadius, door.triggerRadius, from, to,
                           radius, height);
}

Vec3 resolveLegendsTowerVolume(const formats::LegendsN64TowerVolume& volume, const Vec3& from,
                               const Vec3& to, f32 radius, f32 height) {
    if (volume.shape == formats::LegendsN64TowerVolume::Shape::Cylinder) {
        return resolveCylinder(volume.centre, volume.halfExtents.x, volume.halfExtents.y, from, to,
                               radius, height);
    }
    const auto finite = [](const Vec3& p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
    };
    if (!finite(from)) {
        throw std::invalid_argument("Tower volume movement requires a finite starting position");
    }
    if (!finite(to) || !finite(volume.centre) || !finite(volume.halfExtents) ||
        volume.halfExtents.x <= 0 || volume.halfExtents.y <= 0 || volume.halfExtents.z <= 0 ||
        !std::isfinite(volume.yaw) || !std::isfinite(radius) || radius < 0 ||
        !std::isfinite(height) || height < 0) {
        return from;
    }
    // Native category14 (e0048158 case0xd) stops the player's movement. Test
    // a swept circle against its oriented rectangle, not the box's broad sphere.
    const f64 c = std::cos(static_cast<f64>(volume.yaw));
    const f64 s = std::sin(static_cast<f64>(volume.yaw));
    using Point = glm::dvec2;
    const auto local = [&](const Vec3& p) {
        const f64 x = static_cast<f64>(p.x) - volume.centre.x;
        const f64 z = static_cast<f64>(p.z) - volume.centre.z;
        return Point{c * x - s * z, s * x + c * z};
    };
    const Point start = local(from);
    const Point finish = local(to);
    const Point direction = finish - start;
    const Point extent{volume.halfExtents.x, volume.halfExtents.z};
    const auto distance = [&](Point p) {
        const Point gap = glm::max(glm::abs(p) - extent, Point{0});
        return glm::dot(gap, gap);
    };
    const f64 bottom = static_cast<f64>(volume.centre.y) - volume.halfExtents.y - height;
    const f64 top = static_cast<f64>(volume.centre.y) + volume.halfExtents.y;
    const f64 dy = static_cast<f64>(to.y) - from.y;
    f64 enter = 0;
    f64 leave = 1;
    if (dy == 0) {
        if (from.y < bottom || from.y > top) {
            return to;
        }
    } else {
        const f64 a = (bottom - from.y) / dy;
        const f64 b = (top - from.y) / dy;
        enter = std::max(enter, std::min(a, b));
        leave = std::min(leave, std::max(a, b));
    }
    if (enter > leave) {
        return to;
    }
    const f64 radiusSquared = static_cast<f64>(radius) * radius;
    const f64 startDistance = distance(start);
    if (from.y >= bottom && from.y <= top &&
        (startDistance < radiusSquared || startDistance == 0)) {
        const bool horizontalEscape = glm::dot(direction, direction) > 0 &&
                                      glm::dot(start, direction) >= 0 &&
                                      distance(finish) >= startDistance;
        const bool verticalEscape = dy != 0 && glm::dot(direction, direction) == 0 &&
                                    (from.y + height * .5 - volume.centre.y) * dy >= 0;
        return horizontalEscape || verticalEscape ? to : from;
    }
    const Point a = start + enter * direction;
    const Point b = start + leave * direction;
    const Point step = b - a;
    f64 first = 0;
    f64 last = 1;
    bool intersects = true;
    for (int axis = 0; axis < 2; ++axis) {
        if (step[axis] == 0) {
            intersects &= std::abs(a[axis]) <= extent[axis];
        } else {
            const f64 low = (-extent[axis] - a[axis]) / step[axis];
            const f64 high = (extent[axis] - a[axis]) / step[axis];
            first = std::max(first, std::min(low, high));
            last = std::min(last, std::max(low, high));
        }
    }
    if (intersects && first <= last) {
        return from;
    }
    // A closest point is a segment endpoint or a rectangle corner projected
    // onto the segment. This retains rounded corner contacts and clear near misses.
    f64 nearest = std::min(distance(a), distance(b));
    const f64 lengthSquared = glm::dot(step, step);
    for (const f64 x : {-extent.x, extent.x}) {
        for (const f64 z : {-extent.y, extent.y}) {
            const Point corner{x, z};
            const f64 t = lengthSquared == 0
                              ? 0
                              : std::clamp(glm::dot(corner - a, step) / lengthSquared, 0.0, 1.0);
            const Point gap = a + t * step - corner;
            nearest = std::min(nearest, glm::dot(gap, gap));
        }
    }
    return nearest < radiusSquared ? from : to;
}

std::optional<LegendsTowerIdle::Cue> LegendsTowerIdle::update(bool finished, u16 random15) {
    if (random15 > 0x7fff) {
        throw std::invalid_argument("Tower idle random value must be 15-bit");
    }
    if (!finished) {
        return std::nullopt;
    }
    // Native US Tower.bin e08612e0. CNJ1 is followed by one through four
    // complete CNJ2 cycles; CNJ3 returns to ordinary read/page selection.
    if (m_state == State::ConjureStart) {
        m_conjureLoops = static_cast<u8>((random15 >> 13) + 1);
        m_state = State::ConjureLoop;
        return Cue{"CNJ2", {}};
    }
    if (m_state == State::ConjureLoop) {
        if (--m_conjureLoops != 0) {
            return Cue{"CNJ2", {}};
        }
        m_state = State::Reading;
        return Cue{"CNJ3", {}};
    }
    const f32 choice = static_cast<f32>(random15) / 32768.0f;
    if (choice < 0.03f) {
        m_state = State::ConjureStart;
        return Cue{"CNJ1", {}};
    }
    if (choice < 0.2f) {
        return Cue{"PAGE", "FLIP"};
    }
    return Cue{"READ", {}};
}

bool LegendsTowerDoorMotion::unlock() {
    if (m_state != State::Closed) {
        return false;
    }
    m_state = State::Opening;
    return true;
}

void LegendsTowerDoorMotion::update(bool animationFinished) {
    if (m_state == State::Opening && animationFinished) {
        m_state = State::Open;
    }
}

} // namespace gdl::game
