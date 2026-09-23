#include "formats/WorldFile.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr std::size_t kHeaderSize = 96;
constexpr std::size_t kExtendedHeaderSize = 24;
constexpr std::uint32_t kExtendedMagic = 0xF00BAB02;
constexpr std::size_t kObjectSize = 60;
constexpr std::size_t kLocatorSize = 28;
constexpr std::size_t kTriangleSize = 40;
constexpr std::size_t kAnimationSize = 16;
constexpr std::size_t kNameSize = 16;
constexpr std::uint32_t kExtendedHeaderLimit =
    120; ///< a section starting below this leaves no room
constexpr std::size_t kItemNameSize = 16;
constexpr float kOffsetUnit = 1.0f / 64.0f; ///< the triangle's 16-bit offsets are in 1/64 units

/**
 * A collision triangle stores one corner and the other two as offsets in the triangle's own
 * plane: (x, 0, z) tilted about the x axis by the angle between the normal and straight up,
 * then turned about y to face the normal's horizontal direction.
 */
Vec3 planeOffset(const Vec3& normal, float x, float z) {
    const float tilt = std::acos(std::clamp(normal.y, -1.0f, 1.0f));
    const float turn =
        (normal.x == 0.0f && normal.z == 0.0f) ? 0.0f : std::atan2(-normal.x, -normal.z);
    const float sinTilt = std::sin(tilt);
    const float cosTilt = std::cos(tilt);
    const float sinTurn = std::sin(turn);
    const float cosTurn = std::cos(turn);
    return Vec3{x * cosTurn + z * cosTilt * sinTurn, z * sinTilt,
                -x * sinTurn + z * cosTilt * cosTurn};
}

float readF32(ByteReader& reader) {
    return std::bit_cast<float>(reader.readU32());
}

Vec3 readVec3(ByteReader& reader) {
    const float x = readF32(reader);
    const float y = readF32(reader);
    const float z = readF32(reader);
    return Vec3{x, y, z};
}

std::string readName(ByteReader& reader) {
    std::string name;
    bool ended = false;
    for (std::size_t i = 0; i < kNameSize; ++i) {
        const auto c = static_cast<char>(reader.readU8());
        if (c == '\0') {
            ended = true;
        }
        if (!ended) {
            name.push_back(c);
        }
    }
    return name;
}

void checkRange(std::span<const std::uint8_t> bytes, std::uint32_t offset, std::uint32_t count,
                std::size_t stride, const char* what) {
    if (offset > bytes.size() || count > (bytes.size() - offset) / stride) {
        throw FormatError(std::string("world file: ") + what + " out of bounds");
    }
}

/** A fixed-width name, up to its first zero. */
std::string readFixedName(ByteReader& reader, std::size_t width) {
    std::string name;
    for (std::size_t i = 0; i < width; ++i) {
        const auto c = static_cast<char>(reader.readU8());
        if (c != 0 && name.size() == i) {
            name.push_back(c);
        }
    }
    return name;
}

ItemInfoRecord readItemInfo(std::span<const std::uint8_t> record) {
    ByteReader reader(record);
    ItemInfoRecord out;
    out.type = static_cast<std::int32_t>(reader.readU32());
    out.subtype = static_cast<std::int32_t>(reader.readU32());
    out.collisionType = static_cast<std::int16_t>(reader.readU16());
    out.collisionFlags = static_cast<std::int16_t>(reader.readU16());
    out.radius = readF32(reader);
    out.height = readF32(reader);
    out.xSize = readF32(reader);
    out.zSize = readF32(reader);
    out.collisionOffset = readVec3(reader);
    out.name = readFixedName(reader, kItemNameSize);
    out.objectFlags = reader.readU32();
    out.properties = reader.readU32();
    out.value = static_cast<std::int16_t>(reader.readU16());
    out.armor = static_cast<std::int16_t>(reader.readU16());
    out.hitPoints = static_cast<std::int16_t>(reader.readU16());
    out.activeType = static_cast<std::int16_t>(reader.readU16());
    out.activeOff = static_cast<std::int16_t>(reader.readU16());
    out.activeOn = static_cast<std::int16_t>(reader.readU16());
    if (out.type == ItemInfoRecord::kChoiceList) {
        ByteReader list(record.subspan(8));
        const auto count = std::min(static_cast<std::size_t>(std::max(out.subtype, 0)),
                                    ItemInfoRecord::kMostChoices);
        for (std::size_t i = 0; i < count; ++i) {
            out.choices.push_back(static_cast<std::int16_t>(list.readU16()));
        }
    }
    return out;
}

ItemInstanceRecord readItemInstance(std::span<const std::uint8_t> record) {
    ByteReader reader(record);
    ItemInstanceRecord out;
    out.info = static_cast<std::int16_t>(reader.readU16());
    out.minPlayers = static_cast<std::int8_t>(reader.readU8());
    out.flags = reader.readU8();
    out.triangleIndex = static_cast<std::int16_t>(reader.readU16());
    out.triangleCount = static_cast<std::int16_t>(reader.readU16());
    out.name = readFixedName(reader, kItemNameSize);
    out.position = readVec3(reader);
    out.rotation = readVec3(reader);
    for (std::uint8_t& value : out.params) {
        value = reader.readU8();
    }
    return out;
}

} // namespace

WorldFile WorldFile::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize) {
        throw FormatError("world file: too small");
    }
    ByteReader header(bytes);
    WorldFile world;
    const std::uint32_t objectCount = header.readU32();
    const std::uint32_t objectOffset = header.readU32();
    world.collisionTriangleCount = header.readU32();
    const std::uint32_t triangleOffset = header.readU32();
    header.readU32(); // grid entry count
    header.readU32(); // grid entries
    header.readU32(); // grid list count
    header.readU32(); // grid list
    header.readU32(); // grid rows
    world.minBounds = readVec3(header);
    world.maxBounds = readVec3(header);
    world.gridSize = readF32(header);
    world.gridColumns = header.readU32();
    world.gridRows = header.readU32();
    world.itemInfoCount = header.readU32();
    const std::uint32_t itemInfoOffset = header.readU32();
    world.itemInstanceCount = header.readU32();
    const std::uint32_t itemInstanceOffset = header.readU32();
    const std::uint32_t locatorCount = header.readU32();
    const std::uint32_t locatorOffset = header.readU32();

    // An extended header follows unless a section already starts where it would sit.
    const auto startsInside = [](std::uint32_t count, std::uint32_t offset) {
        return count > 0 && offset < kExtendedHeaderLimit;
    };
    const bool extended = bytes.size() >= kHeaderSize + kExtendedHeaderSize &&
                          !startsInside(objectCount, objectOffset) &&
                          !startsInside(world.itemInfoCount, itemInfoOffset) &&
                          !startsInside(world.itemInstanceCount, itemInstanceOffset) &&
                          !startsInside(locatorCount, locatorOffset);
    std::uint32_t keyHeaderOffset = 0;
    std::uint32_t animationOffset = 0;
    std::uint32_t particleOffset = 0;
    if (extended) {
        ByteReader extra(bytes.subspan(kHeaderSize, kExtendedHeaderSize));
        if (extra.readU32() != kExtendedMagic) {
            throw FormatError("world file: unknown extended header");
        }
        keyHeaderOffset = extra.readU32();
        world.animationCount = extra.readU32();
        animationOffset = extra.readU32();
        world.particleSystemCount = extra.readU32();
        particleOffset = extra.readU32();
    }
    if (world.particleSystemCount > 0 && particleOffset != 0) {
        checkRange(bytes, particleOffset, world.particleSystemCount, ParticleTemplateRecord::kSize,
                   "particle templates");
        for (std::uint32_t i = 0; i < world.particleSystemCount; ++i) {
            world.particles.push_back(readParticleTemplate(
                bytes.subspan(particleOffset + std::size_t{i} * ParticleTemplateRecord::kSize,
                              ParticleTemplateRecord::kSize)));
        }
    }

    if (world.animationCount > 0 && keyHeaderOffset != 0) {
        checkRange(bytes, animationOffset, world.animationCount, kAnimationSize, "animations");
        const KeyHeader keys = readKeyHeader(bytes, keyHeaderOffset, "world file key header");
        for (std::uint32_t i = 0; i < world.animationCount; ++i) {
            ByteReader reader(
                bytes.subspan(animationOffset + std::size_t{i} * kAnimationSize, kAnimationSize));
            WorldAnimationRecord animation;
            animation.objectIndex = static_cast<std::int16_t>(reader.readU16());
            animation.frameCount = static_cast<std::int16_t>(reader.readU16());
            reader.readU16(); // relocation mark
            animation.state = reader.readU16();
            animation.startFrame = readF32(reader);
            const std::uint32_t entry = reader.readU32();
            animation.track =
                readKeyTrack(bytes, entry, keys, animation.frameCount, "world file animation");
            world.animations.push_back(std::move(animation));
        }
    }

    if (world.itemInfoCount > 0) {
        checkRange(bytes, itemInfoOffset, world.itemInfoCount, ItemInfoRecord::kSize, "item infos");
        for (std::uint32_t i = 0; i < world.itemInfoCount; ++i) {
            world.itemInfos.push_back(readItemInfo(bytes.subspan(
                itemInfoOffset + std::size_t{i} * ItemInfoRecord::kSize, ItemInfoRecord::kSize)));
        }
    }
    if (world.itemInstanceCount > 0) {
        checkRange(bytes, itemInstanceOffset, world.itemInstanceCount, ItemInstanceRecord::kSize,
                   "item instances");
        for (std::uint32_t i = 0; i < world.itemInstanceCount; ++i) {
            world.itemInstances.push_back(readItemInstance(
                bytes.subspan(itemInstanceOffset + std::size_t{i} * ItemInstanceRecord::kSize,
                              ItemInstanceRecord::kSize)));
        }
    }

    checkRange(bytes, objectOffset, objectCount, kObjectSize, "objects");
    world.objects.reserve(objectCount);
    for (std::uint32_t i = 0; i < objectCount; ++i) {
        ByteReader reader(bytes.subspan(objectOffset + std::size_t{i} * kObjectSize, kObjectSize));
        WorldObjectRecord object;
        object.name = readName(reader);
        object.flags = reader.readU32();
        reader.readU32(); // trigger type and states, always zero on disc
        object.objectFlags = reader.readU32();
        object.position = readVec3(reader);
        reader.readU32(); // runtime node pointer
        object.nextIndex = static_cast<std::int16_t>(reader.readU16());
        object.childIndex = static_cast<std::int16_t>(reader.readU16());
        object.radius = readF32(reader);
        reader.readU8(); // checked
        object.noCollision = reader.readU8() != 0;
        object.collisionTriangleCount = static_cast<std::int16_t>(reader.readU16());
        object.collisionTriangleIndex = reader.readS32();
        world.objects.push_back(std::move(object));
    }

    checkRange(bytes, triangleOffset, world.collisionTriangleCount, kTriangleSize,
               "collision triangles");
    world.collision.reserve(world.collisionTriangleCount);
    for (std::uint32_t i = 0; i < world.collisionTriangleCount; ++i) {
        ByteReader reader(
            bytes.subspan(triangleOffset + std::size_t{i} * kTriangleSize, kTriangleSize));
        reader.readU16(); // lowest y, in 1/64 units: a search bound the reader recomputes
        reader.readU16(); // highest y
        reader.readU32(); // 1 / the normal's horizontal length, implied by the normal
        WorldCollisionTriangle triangle;
        triangle.normal = readVec3(reader);
        triangle.vertices[0] = readVec3(reader);
        const auto v1x = static_cast<std::int16_t>(reader.readU16());
        const auto v1z = static_cast<std::int16_t>(reader.readU16());
        const auto v2x = static_cast<std::int16_t>(reader.readU16());
        const auto v2z = static_cast<std::int16_t>(reader.readU16());
        triangle.vertices[1] = triangle.vertices[0] +
                               planeOffset(triangle.normal, static_cast<float>(v1x) * kOffsetUnit,
                                           static_cast<float>(v1z) * kOffsetUnit);
        triangle.vertices[2] = triangle.vertices[0] +
                               planeOffset(triangle.normal, static_cast<float>(v2x) * kOffsetUnit,
                                           static_cast<float>(v2z) * kOffsetUnit);
        world.collision.push_back(triangle);
    }

    checkRange(bytes, locatorOffset, locatorCount, kLocatorSize, "locators");
    world.locators.reserve(locatorCount);
    for (std::uint32_t i = 0; i < locatorCount; ++i) {
        ByteReader reader(
            bytes.subspan(locatorOffset + std::size_t{i} * kLocatorSize, kLocatorSize));
        WorldLocatorRecord locator;
        const std::uint8_t kind = reader.readU8();
        locator.kind = kind < static_cast<std::uint8_t>(LocatorKind::Count)
                           ? static_cast<LocatorKind>(kind)
                           : LocatorKind::None;
        locator.delay = reader.readU8();
        locator.next = reader.readU16();
        locator.position = readVec3(reader);
        locator.rotation = readVec3(reader);
        world.locators.push_back(locator);
    }
    return world;
}

} // namespace gdl::formats
