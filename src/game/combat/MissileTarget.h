#pragma once

#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCollision.h"

namespace gdl::game {
struct StrikeHit;

/** Something a missile stops against: a cylinder, or an authored triangle surface. */
struct MissileTarget {
    /** Authored acquisition geometry, separate from what a missile can collide with. */
    struct Acquisition {
        Vec3 point{0};
        f32 radius = 0;
        f32 distanceScale = 1;
        f32 maxHeight = 0;
        bool enabled = true; ///< automatic selection only; never removes physical contacts
    };

    s32 id = -1;
    Vec3 base{0.0f, 0.0f, 0.0f};
    f32 radius = 1.0f;
    f32 height = 1.0f;
    // The explicit default lets cylinder-only aggregate initializers omit geometry without
    // -Wmissing-field-initializers, while mesh targets borrow surfaces for the query.
    std::span<const CollisionTriangle>
        surface{}; // NOLINT(readability-redundant-member-init): intentional aggregate default
    s32 node = -1; ///< collision-part index; id still identifies the owning creature
    f32 targetScoreScale = 1.0f; ///< for choosing between one creature's live parts
    f32 maxTargetDistance = 0.0f;
    bool potionBottle = false;
    std::optional<Acquisition> acquisition = std::nullopt;
    Vec3 pointNear(const Vec3& point) const;
    bool touches(const Vec3& point, f32 reach) const;
    bool reachedBy(const StrikeHit& strike) const;
};
} // namespace gdl::game
