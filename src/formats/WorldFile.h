#pragma once

#include <span>
#include <string>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::formats {

/** One placed object as the level file stores it. */
struct WorldObjectRecord {
    std::string name;
    u32 flags = 0;
    u32 objectFlags = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    s16 nextIndex = -1;
    s16 childIndex = -1;
    f32 radius = 0.0f;
    s16 collisionTriangleCount = 0;
    s32 collisionTriangleIndex = 0;
};

struct WorldLocatorRecord {
    LocatorKind kind = LocatorKind::None;
    u8 delay = 0;
    u16 next = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f};
};

/**
 * A level's WORLDS.PS2: the placed objects, their hierarchy and the marker points, plus the
 * counts of the sections this reader leaves alone (collision, items, animations).
 */
struct WorldFile {
    Vec3 minBounds{0.0f, 0.0f, 0.0f};
    Vec3 maxBounds{0.0f, 0.0f, 0.0f};
    f32 gridSize = 0.0f;
    u32 gridColumns = 0;
    u32 gridRows = 0;
    u32 collisionTriangleCount = 0;
    u32 itemInfoCount = 0;
    u32 itemInstanceCount = 0;
    u32 animationCount = 0;
    u32 particleSystemCount = 0;
    std::vector<WorldObjectRecord> objects;
    std::vector<WorldLocatorRecord> locators;

    /** Parses the little-endian file; throws FormatError when malformed. */
    static WorldFile parse(std::span<const u8> bytes);
};

} // namespace gdl::formats
