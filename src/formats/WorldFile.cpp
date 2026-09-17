#include "formats/WorldFile.h"

#include <bit>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr usize kHeaderSize = 96;
constexpr usize kExtendedHeaderSize = 24;
constexpr u32 kExtendedMagic = 0xF00BAB02;
constexpr usize kObjectSize = 60;
constexpr usize kLocatorSize = 28;
constexpr usize kNameSize = 16;
constexpr u32 kExtendedHeaderLimit = 120; ///< a section starting below this leaves no room

f32 readF32(ByteReader& reader) {
    return std::bit_cast<f32>(reader.readU32());
}

Vec3 readVec3(ByteReader& reader) {
    const f32 x = readF32(reader);
    const f32 y = readF32(reader);
    const f32 z = readF32(reader);
    return Vec3{x, y, z};
}

std::string readName(ByteReader& reader) {
    std::string name;
    bool ended = false;
    for (usize i = 0; i < kNameSize; ++i) {
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

void checkRange(std::span<const u8> bytes, u32 offset, u32 count, usize stride, const char* what) {
    if (offset > bytes.size() || count > (bytes.size() - offset) / stride) {
        throw FormatError(std::string("world file: ") + what + " out of bounds");
    }
}

} // namespace

WorldFile WorldFile::parse(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderSize) {
        throw FormatError("world file: too small");
    }
    ByteReader header(bytes);
    WorldFile world;
    const u32 objectCount = header.readU32();
    const u32 objectOffset = header.readU32();
    world.collisionTriangleCount = header.readU32();
    header.readU32(); // collision triangles
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
    const u32 itemInfoOffset = header.readU32();
    world.itemInstanceCount = header.readU32();
    const u32 itemInstanceOffset = header.readU32();
    const u32 locatorCount = header.readU32();
    const u32 locatorOffset = header.readU32();

    // An extended header follows unless a section already starts where it would sit.
    const auto startsInside = [](u32 count, u32 offset) {
        return count > 0 && offset < kExtendedHeaderLimit;
    };
    const bool extended = bytes.size() >= kHeaderSize + kExtendedHeaderSize &&
                          !startsInside(objectCount, objectOffset) &&
                          !startsInside(world.itemInfoCount, itemInfoOffset) &&
                          !startsInside(world.itemInstanceCount, itemInstanceOffset) &&
                          !startsInside(locatorCount, locatorOffset);
    if (extended) {
        ByteReader extra(bytes.subspan(kHeaderSize, kExtendedHeaderSize));
        if (extra.readU32() != kExtendedMagic) {
            throw FormatError("world file: unknown extended header");
        }
        extra.readU32(); // animation header
        world.animationCount = extra.readU32();
        extra.readU32(); // animations
        world.particleSystemCount = extra.readU32();
    }

    checkRange(bytes, objectOffset, objectCount, kObjectSize, "objects");
    world.objects.reserve(objectCount);
    for (u32 i = 0; i < objectCount; ++i) {
        ByteReader reader(bytes.subspan(objectOffset + usize{i} * kObjectSize, kObjectSize));
        WorldObjectRecord object;
        object.name = readName(reader);
        object.flags = reader.readU32();
        reader.readU32(); // trigger type and states, always zero on disc
        object.objectFlags = reader.readU32();
        object.position = readVec3(reader);
        reader.readU32(); // runtime node pointer
        object.nextIndex = static_cast<s16>(reader.readU16());
        object.childIndex = static_cast<s16>(reader.readU16());
        object.radius = readF32(reader);
        reader.readU8(); // checked
        reader.readU8(); // no collision
        object.collisionTriangleCount = static_cast<s16>(reader.readU16());
        object.collisionTriangleIndex = reader.readS32();
        world.objects.push_back(std::move(object));
    }

    checkRange(bytes, locatorOffset, locatorCount, kLocatorSize, "locators");
    world.locators.reserve(locatorCount);
    for (u32 i = 0; i < locatorCount; ++i) {
        ByteReader reader(bytes.subspan(locatorOffset + usize{i} * kLocatorSize, kLocatorSize));
        WorldLocatorRecord locator;
        const u8 kind = reader.readU8();
        locator.kind = kind < static_cast<u8>(LocatorKind::Count) ? static_cast<LocatorKind>(kind)
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
