#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/math/Math.h"

#include "formats/KeyframeTrack.h"
#include "formats/ParticleTemplate.h"

namespace gdl::formats {

/** One placed object as the level file stores it. */
struct WorldObjectRecord {
    std::string name;
    std::uint32_t flags = 0;
    std::uint32_t objectFlags = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    std::int16_t nextIndex = -1;
    std::int16_t childIndex = -1;
    float radius = 0.0f;
    bool noCollision = false;
    std::int16_t collisionTriangleCount = 0;
    std::int32_t collisionTriangleIndex = 0;
};

struct WorldLocatorRecord {
    LocatorKind kind = LocatorKind::None;
    std::uint8_t delay = 0;
    std::uint16_t next = 0;
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
    std::int32_t objectIndex = -1;
    std::int32_t frameCount = 0;
    std::uint32_t state = 0;
    float startFrame = 0.0f;
    NodeTrack track;
};

/** One kind of item a level places: its type and subtype, collision shape, name, display
 * flags and combat values, as the file stores them. */
struct ItemInfoRecord {
    static constexpr std::size_t kSize = 0x50;

    std::int32_t type = 0;
    std::int32_t subtype = 0;
    std::int16_t collisionType = 0;
    std::int16_t collisionFlags = 0;
    float radius = 0.0f;
    float height = 0.0f;
    float xSize = 0.0f;
    float zSize = 0.0f;
    Vec3 collisionOffset{0.0f, 0.0f, 0.0f};
    std::string name;
    std::uint32_t objectFlags = 0;
    std::uint32_t properties = 0;
    std::int16_t value = 0;
    std::int16_t armor = 0;
    std::int16_t hitPoints = 0;
    std::int16_t activeType = 0;
    std::int16_t activeOff = 0;
    std::int16_t activeOn = 0;
    /** A record of type -1 is no item but a list to pick one from at random: `subtype` of
     * these indices into the item records, stored where an item keeps its collision. */
    std::vector<std::int16_t> choices;

    static constexpr std::int32_t kChoiceList = -1;
    static constexpr std::size_t kMostChoices = 36; ///< what fits before the record's end
};

/** One item the level places: which kind, how many players it takes, its own name when it
 * has one, where it stands and the kind's parameters. */
struct ItemInstanceRecord {
    static constexpr std::size_t kSize = 0x3C;

    std::int16_t info = -1;
    std::int8_t minPlayers = 0;
    std::uint8_t flags = 0;
    std::int16_t triangleIndex = -1;
    std::int16_t triangleCount = 0;
    std::string name;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f}; ///< pitch, yaw, roll
    std::array<std::uint8_t, 12> params{};
};

/**
 * A level's WORLDS.PS2: the placed objects, their hierarchy, the marker points, the collision
 * triangles, the objects' keyframe animations, the particle templates its markers start,
 * and the items it places.
 */
struct WorldFile {
    Vec3 minBounds{0.0f, 0.0f, 0.0f};
    Vec3 maxBounds{0.0f, 0.0f, 0.0f};
    float gridSize = 0.0f;
    std::uint32_t gridColumns = 0;
    std::uint32_t gridRows = 0;
    std::uint32_t collisionTriangleCount = 0;
    std::uint32_t itemInfoCount = 0;
    std::uint32_t itemInstanceCount = 0;
    std::uint32_t animationCount = 0;
    std::uint32_t particleSystemCount = 0;
    std::vector<WorldObjectRecord> objects;
    std::vector<WorldLocatorRecord> locators;
    std::vector<WorldCollisionTriangle> collision; ///< indexed by the objects' ranges
    std::vector<WorldAnimationRecord> animations;
    std::vector<ParticleTemplateRecord> particles;
    std::vector<ItemInfoRecord> itemInfos;
    std::vector<ItemInstanceRecord> itemInstances;

    /** Parses the little-endian file; throws FormatError when malformed. */
    static WorldFile parse(std::span<const std::uint8_t> bytes);
};

} // namespace gdl::formats
