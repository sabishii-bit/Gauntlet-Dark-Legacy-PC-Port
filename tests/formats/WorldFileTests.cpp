#include <bit>
#include <cstring>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"

#include "formats/WorldFile.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

constexpr usize kHeader = 96;
constexpr usize kExtended = 24;
constexpr usize kObject = 60;
constexpr usize kLocator = 28;

void put32(std::vector<u8>& bytes, usize offset, u32 value) {
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    std::memcpy(&bytes[offset], &value, 4);
}

void put16(std::vector<u8>& bytes, usize offset, u16 value) {
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    std::memcpy(&bytes[offset], &value, 2);
}

void putF(std::vector<u8>& bytes, usize offset, f32 value) {
    put32(bytes, offset, std::bit_cast<u32>(value));
}

void putName(std::vector<u8>& bytes, usize offset, std::string_view name) {
    std::memcpy(&bytes[offset], name.data(), name.size());
}

/** Two objects (a group with one child mesh) and one camera, with the extended header. */
std::vector<u8> sampleWorld(bool extended) {
    const usize objects = extended ? kHeader + kExtended : kHeader;
    const usize locators = objects + 2 * kObject;
    std::vector<u8> bytes(locators + kLocator, 0);
    put32(bytes, 0, 2);
    put32(bytes, 4, static_cast<u32>(objects));
    put32(bytes, 8, 288); // collision triangles
    putF(bytes, 36, -61.1f);
    putF(bytes, 40, -50.9f);
    putF(bytes, 44, -50.0f);
    putF(bytes, 48, 249.9f);
    putF(bytes, 52, 17.9f);
    putF(bytes, 56, 50.0f);
    putF(bytes, 60, 8.0f);
    put32(bytes, 64, 39);
    put32(bytes, 68, 13);
    put32(bytes, 72, 104); // item infos
    put32(bytes, 76, static_cast<u32>(locators));
    put32(bytes, 80, 40); // item instances
    put32(bytes, 84, static_cast<u32>(locators));
    put32(bytes, 88, 1);
    put32(bytes, 92, static_cast<u32>(locators));
    if (extended) {
        put32(bytes, kHeader, 0xF00BAB02);
        put32(bytes, kHeader + 8, 7);
        put32(bytes, kHeader + 16, 4);
    }
    // object 0: a group whose child is object 1
    putName(bytes, objects, "T1FLOOR1");
    put32(bytes, objects + 16, 0x4);
    putF(bytes, objects + 28, 1.0f);
    putF(bytes, objects + 32, 2.0f);
    putF(bytes, objects + 36, 3.0f);
    put16(bytes, objects + 44, 0xFFFF); // next -1
    put16(bytes, objects + 46, 1);      // child
    // object 1: the mesh
    const usize second = objects + kObject;
    putName(bytes, second, "T1#0");
    put32(bytes, second + 16, 0x6);
    put32(bytes, second + 24, 0x8000);
    putF(bytes, second + 28, 0.5f);
    put16(bytes, second + 44, 0xFFFF);
    put16(bytes, second + 46, 0xFFFF);
    putF(bytes, second + 48, 70.7f);
    put16(bytes, second + 54, 12);
    put32(bytes, second + 56, 100);
    // locator: camera_start
    bytes[locators] = 1;
    bytes[locators + 1] = 5;
    put16(bytes, locators + 2, 2);
    putF(bytes, locators + 4, 3.59f);
    putF(bytes, locators + 8, 20.52f);
    putF(bytes, locators + 12, -9.23f);
    putF(bytes, locators + 16, 0.551f);
    putF(bytes, locators + 20, 3.138f);
    return bytes;
}

TEST_CASE("a world file yields its objects, hierarchy and locators", "[formats][world]") {
    const WorldFile world = WorldFile::parse(sampleWorld(true));
    REQUIRE(world.objects.size() == 2);
    REQUIRE(world.locators.size() == 1);
    REQUIRE(world.collisionTriangleCount == 288);
    REQUIRE(world.itemInfoCount == 104);
    REQUIRE(world.itemInstanceCount == 40);
    REQUIRE(world.animationCount == 7);
    REQUIRE(world.particleSystemCount == 4);
    REQUIRE(world.minBounds.x == -61.1f);
    REQUIRE(world.maxBounds.z == 50.0f);
    REQUIRE(world.gridSize == 8.0f);
    REQUIRE(world.gridColumns == 39);
    REQUIRE(world.gridRows == 13);

    const WorldObjectRecord& group = world.objects[0];
    REQUIRE(group.name == "T1FLOOR1");
    REQUIRE(group.flags == 0x4);
    REQUIRE(group.position == Vec3{1.0f, 2.0f, 3.0f});
    REQUIRE(group.nextIndex == -1);
    REQUIRE(group.childIndex == 1);
    const WorldObjectRecord& mesh = world.objects[1];
    REQUIRE(mesh.name == "T1#0");
    REQUIRE(mesh.objectFlags == 0x8000);
    REQUIRE(mesh.position.x == 0.5f);
    REQUIRE(mesh.childIndex == -1);
    REQUIRE(mesh.radius == 70.7f);
    REQUIRE(mesh.collisionTriangleCount == 12);
    REQUIRE(mesh.collisionTriangleIndex == 100);

    const WorldLocatorRecord& camera = world.locators[0];
    REQUIRE(camera.kind == LocatorKind::CameraStart);
    REQUIRE(camera.delay == 5);
    REQUIRE(camera.next == 2);
    REQUIRE(camera.position == Vec3{3.59f, 20.52f, -9.23f});
    REQUIRE(camera.rotation.x == 0.551f);
    REQUIRE(camera.rotation.y == 3.138f);
}

TEST_CASE("a world file without the extended header still parses", "[formats][world]") {
    const WorldFile world = WorldFile::parse(sampleWorld(false));
    REQUIRE(world.objects.size() == 2);
    REQUIRE(world.animationCount == 0);
    REQUIRE(world.objects[0].name == "T1FLOOR1");
}

TEST_CASE("damaged world files are rejected", "[formats][world]") {
    std::vector<u8> bytes = sampleWorld(true);
    bytes.resize(50);
    REQUIRE_THROWS_AS(WorldFile::parse(bytes), FormatError);

    bytes = sampleWorld(true);
    put32(bytes, kHeader, 0x12345678);
    REQUIRE_THROWS_AS(WorldFile::parse(bytes), FormatError);

    bytes = sampleWorld(true);
    put32(bytes, 0, 1000);
    REQUIRE_THROWS_AS(WorldFile::parse(bytes), FormatError);

    bytes = sampleWorld(true);
    put32(bytes, 92, 100000);
    REQUIRE_THROWS_AS(WorldFile::parse(bytes), FormatError);
}

} // namespace
