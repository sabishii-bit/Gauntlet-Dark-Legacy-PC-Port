#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "formats/KeyframeTrack.h"
#include "formats/ParticleTemplate.h"

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
    bool noCollision = false;
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

/** One collision triangle, in world space (the owning object's placement is already
 * applied). */
struct WorldCollisionTriangle {
    Vec3 normal{0.0f, 1.0f, 0.0f};
    std::array<Vec3, 3> vertices{};
};

/** One placed object's keyframes: its frame count and the channels it moves. */
struct WorldAnimationRecord {
    s32 objectIndex = -1;
    s32 frameCount = 0;
    u32 state = 0;
    f32 startFrame = 0.0f;
    NodeTrack track;
};

/** One kind of item a level places: its type and subtype, collision shape, name, display
 * flags and combat values, as the file stores them. */
struct ItemInfoRecord {
    static constexpr usize kSize = 0x50;

    s32 type = 0;
    s32 subtype = 0;
    s16 collisionType = 0;
    s16 collisionFlags = 0;
    f32 radius = 0.0f;
    f32 height = 0.0f;
    f32 xSize = 0.0f;
    f32 zSize = 0.0f;
    Vec3 collisionOffset{0.0f, 0.0f, 0.0f};
    std::string name;
    u32 objectFlags = 0;
    u32 properties = 0;
    s16 value = 0;
    s16 armor = 0;
    s16 hitPoints = 0;
    s16 activeType = 0;
    s16 activeOff = 0;
    s16 activeOn = 0;
    /** A record of type -1 is no item but a list to pick one from at random: `subtype` of
     * these indices into the item records, stored where an item keeps its collision. */
    std::vector<s16> choices;

    static constexpr s32 kChoiceList = -1;
    static constexpr usize kMostChoices = 36; ///< what fits before the record's end
};

/** One item the level places: which kind, how many players it takes, its own name when it
 * has one, where it stands and the kind's parameters. */
struct ItemInstanceRecord {
    static constexpr usize kSize = 0x3C;

    s16 info = -1;
    s8 minPlayers = 0;
    u8 flags = 0;
    s16 triangleIndex = -1;
    s16 triangleCount = 0;
    std::string name;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f}; ///< pitch, yaw, roll
    std::array<u8, 12> params{};
};

/**
 * A level's WORLDS.PS2: the placed objects, their hierarchy, the marker points, the collision
 * triangles, the objects' keyframe animations, the particle templates its markers start,
 * and the items it places.
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
    std::vector<WorldCollisionTriangle> collision; ///< indexed by the objects' ranges
    std::vector<WorldAnimationRecord> animations;
    std::vector<ParticleTemplateRecord> particles;
    std::vector<ItemInfoRecord> itemInfos;
    std::vector<ItemInstanceRecord> itemInstances;

    /** Parses the little-endian file; throws FormatError when malformed. */
    static WorldFile parse(std::span<const u8> bytes);
};

} // namespace gdl::formats
