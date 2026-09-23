#include <bit>
#include <cstring>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/WorldFile.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using Catch::Approx;
using Catch::Matchers::WithinAbs;

constexpr usize kHeader = 96;
constexpr usize kExtended = 24;
constexpr usize kObject = 60;
constexpr usize kLocator = 28;
constexpr usize kTriangle = 40;

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

constexpr usize kParticle = 0x138;
constexpr usize kItemInfo = 0x50;
constexpr usize kItemInstance = 0x3C;

/** Two objects (a group with one child mesh), one camera and one wall triangle, with the
 * extended header naming one animation (the mesh turning about z over four frames) and one
 * particle template: torch flames from preset 5. */
std::vector<u8> sampleWorld(bool extended) {
    const usize objects = extended ? kHeader + kExtended : kHeader;
    const usize locators = objects + 2 * kObject;
    const usize triangles = locators + kLocator;
    const usize keys = triangles + kTriangle; // the key header, its entry, then the keys
    const usize animation = keys + 28 + 8 + 12;
    const usize particle = animation + 16;
    const usize itemInfo = extended ? particle + kParticle : triangles + kTriangle;
    const usize itemInstance = itemInfo + kItemInfo;
    std::vector<u8> bytes(itemInstance + kItemInstance, 0);
    put32(bytes, 0, 2);
    put32(bytes, 4, static_cast<u32>(objects));
    put32(bytes, 8, 1); // collision triangles
    put32(bytes, 12, static_cast<u32>(triangles));
    putF(bytes, 36, -61.1f);
    putF(bytes, 40, -50.9f);
    putF(bytes, 44, -50.0f);
    putF(bytes, 48, 249.9f);
    putF(bytes, 52, 17.9f);
    putF(bytes, 56, 50.0f);
    putF(bytes, 60, 8.0f);
    put32(bytes, 64, 39);
    put32(bytes, 68, 13);
    // One kind of item, an orange crystal, placed once for a party of one.
    put32(bytes, 72, 1);
    put32(bytes, 76, static_cast<u32>(itemInfo));
    put32(bytes, 80, 1);
    put32(bytes, 84, static_cast<u32>(itemInstance));
    put32(bytes, 88, 1);
    put32(bytes, 92, static_cast<u32>(locators));
    put32(bytes, itemInfo, 1);
    put32(bytes, itemInfo + 4, 15);
    put16(bytes, itemInfo + 8, 1);
    putF(bytes, itemInfo + 12, 0.1f);
    putF(bytes, itemInfo + 16, 2.0f);
    putName(bytes, itemInfo + 0x28, "GEMORANGE");
    put16(bytes, itemInfo + 0x40, 4);
    put16(bytes, itemInfo + 0x42, static_cast<u16>(-1));
    put16(bytes, itemInfo + 0x46, 16);
    put16(bytes, itemInstance, 0);
    bytes[itemInstance + 2] = 1;
    putF(bytes, itemInstance + 0x18, 19.3f);
    putF(bytes, itemInstance + 0x1C, -2.0f);
    putF(bytes, itemInstance + 0x20, -57.5f);
    putF(bytes, itemInstance + 0x28, 1.5f);
    bytes[itemInstance + 0x30] = 7;
    if (extended) {
        put32(bytes, kHeader, 0xF00BAB02);
        put32(bytes, kHeader + 4, static_cast<u32>(keys));
        put32(bytes, kHeader + 8, 1);
        put32(bytes, kHeader + 12, static_cast<u32>(animation));
        put32(bytes, kHeader + 16, 1);
        put32(bytes, kHeader + 20, static_cast<u32>(particle));
        put32(bytes, keys + 12, 36); // blocks follow the header and its one entry
        put32(bytes, keys + 20, 1);
        put32(bytes, keys + 24, 1);
        put16(bytes, keys + 28, 0x0004); // rotation z, one float a key
        put16(bytes, keys + 30, 1);
        put32(bytes, keys + 36, 0x9); // keys at frames 0 and 3
        putF(bytes, keys + 40, 0.0f);
        putF(bytes, keys + 44, 1.5f);
        put16(bytes, animation, 1); // the mesh
        put16(bytes, animation + 2, 4);
        put16(bytes, animation + 6, 0x101);
        put32(bytes, animation + 12, static_cast<u32>(keys + 28));
        put32(bytes, particle, 0x101);
        put16(bytes, particle + 4, 5);
        bytes[particle + 6] = static_cast<u8>('E');
        put32(bytes, particle + 8, 0x288);
        put32(bytes, particle + 12, 0x288);
        put32(bytes, particle + 16, 0x56BE1);
        putF(bytes, particle + 0x28, 0.2f);
        putF(bytes, particle + 0x2C, 0.22f);
        putF(bytes, particle + 0x38, 80.0f);
        putName(bytes, particle + 0x40, "P_TORCH");
        putF(bytes, particle + 0x64, 1.0f);
        putF(bytes, particle + 0x6C, 0.1f);
        putF(bytes, particle + 0x70, 0.3f);
        putF(bytes, particle + 0x74, 0.1f);
        for (usize i = 0; i < 4; ++i) {
            putF(bytes, particle + 0x78 + i * 4, 25.0f);
        }
        putF(bytes, particle + 0x8C, -0.38f);
        putF(bytes, particle + 0x94, 4.0f);
        put32(bytes, particle + 0x9C, 0x00FFFFFF);
        put32(bytes, particle + 0xA0, 0x00FFFFFF);
        putF(bytes, particle + 0xA8, 2.8f);
        putF(bytes, particle + 0xAC, 2.0f);
        putF(bytes, particle + 0xB0, 2.0f);
        putF(bytes, particle + 0xB4, 0.1f);
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
    bytes[second + 53] = 1; // no collision
    put16(bytes, second + 54, 1);
    put32(bytes, second + 56, 0);
    // locator: camera_start
    bytes[locators] = 1;
    bytes[locators + 1] = 5;
    put16(bytes, locators + 2, 2);
    putF(bytes, locators + 4, 3.59f);
    putF(bytes, locators + 8, 20.52f);
    putF(bytes, locators + 12, -9.23f);
    putF(bytes, locators + 16, 0.551f);
    putF(bytes, locators + 20, 3.138f);
    // A wall facing +z with its corner at (0, 1, 0), 1 unit wide and 1.75 tall (1/64 units).
    put16(bytes, triangles, static_cast<u16>(-48));
    put16(bytes, triangles + 2, 64);
    putF(bytes, triangles + 4, 1.0f);
    putF(bytes, triangles + 16, 1.0f); // normal z
    putF(bytes, triangles + 24, 1.0f); // corner y
    put16(bytes, triangles + 32, 64);
    put16(bytes, triangles + 34, 0);
    put16(bytes, triangles + 36, 0);
    put16(bytes, triangles + 38, static_cast<u16>(-112));
    return bytes;
}

TEST_CASE("a world file yields its objects, hierarchy and locators", "[formats][world]") {
    const WorldFile world = WorldFile::parse(sampleWorld(true));
    REQUIRE(world.objects.size() == 2);
    REQUIRE(world.locators.size() == 1);
    REQUIRE(world.collisionTriangleCount == 1);
    REQUIRE(world.itemInfoCount == 1);
    REQUIRE(world.itemInstanceCount == 1);
    REQUIRE(world.itemInfos.size() == 1);
    const ItemInfoRecord& gem = world.itemInfos[0];
    REQUIRE(gem.type == 1);
    REQUIRE(gem.subtype == 15);
    REQUIRE(gem.collisionType == 1);
    REQUIRE(gem.radius == 0.1f);
    REQUIRE(gem.height == 2.0f);
    REQUIRE(gem.name == "GEMORANGE");
    REQUIRE(gem.value == 4);
    REQUIRE(gem.armor == -1);
    REQUIRE(gem.activeType == 16);
    REQUIRE(world.itemInstances.size() == 1);
    const ItemInstanceRecord& placed = world.itemInstances[0];
    REQUIRE(placed.info == 0);
    REQUIRE(placed.minPlayers == 1);
    REQUIRE(placed.name.empty());
    REQUIRE(placed.position == Vec3{19.3f, -2.0f, -57.5f});
    REQUIRE(placed.rotation == Vec3{0.0f, 1.5f, 0.0f});
    REQUIRE(placed.params[0] == 7);
    REQUIRE(world.animationCount == 1);
    REQUIRE(world.particleSystemCount == 1);
    REQUIRE(world.particles.size() == 1);
    const ParticleTemplateRecord& torch = world.particles[0];
    REQUIRE(torch.id == 'E');
    REQUIRE(torch.preset == 5);
    REQUIRE(torch.flags == 0x288);
    REQUIRE(torch.enables == 0x56BE1);
    REQUIRE(torch.particleLife == std::array<f32, 2>{0.2f, 0.22f});
    REQUIRE(torch.angle == 80.0f);
    REQUIRE(torch.texture == "P_TORCH");
    REQUIRE(torch.direction == Vec3{0.0f, 1.0f, 0.0f});
    REQUIRE(torch.volume == Vec3{0.1f, 0.3f, 0.1f});
    REQUIRE(torch.rate[0] == 25.0f);
    REQUIRE(torch.gravity == -0.38f);
    REQUIRE(torch.speed == 4.0f);
    REQUIRE(torch.rgba == std::array<u32, 4>{0, 0x00FFFFFF, 0x00FFFFFF, 0});
    REQUIRE(torch.width == std::array<f32, 4>{2.8f, 2.0f, 2.0f, 0.1f});
    REQUIRE(world.animations.size() == 1);
    REQUIRE(world.animations[0].objectIndex == 1);
    REQUIRE(world.animations[0].frameCount == 4);
    REQUIRE(world.animations[0].state == 0x101);
    REQUIRE(world.animations[0].track.flags == NodeTrack::kRotationZ);
    REQUIRE(world.animations[0].track.frames == std::vector<u16>{0, 3});
    REQUIRE(world.animations[0].track.values == std::vector<f32>{0.0f, 1.5f});
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
    REQUIRE(mesh.noCollision);
    REQUIRE_FALSE(group.noCollision);
    REQUIRE(mesh.collisionTriangleCount == 1);
    REQUIRE(mesh.collisionTriangleIndex == 0);

    // The wall's offsets unfold in its plane: x runs along the wall, z runs down it.
    REQUIRE(world.collision.size() == 1);
    const WorldCollisionTriangle& wall = world.collision[0];
    REQUIRE(wall.normal == Vec3{0.0f, 0.0f, 1.0f});
    REQUIRE(wall.vertices[0] == Vec3{0.0f, 1.0f, 0.0f});
    REQUIRE_THAT(wall.vertices[1].x, WithinAbs(-1.0, 1e-5));
    REQUIRE_THAT(wall.vertices[1].y, WithinAbs(1.0, 1e-5));
    REQUIRE_THAT(wall.vertices[1].z, WithinAbs(0.0, 1e-5));
    REQUIRE_THAT(wall.vertices[2].x, WithinAbs(0.0, 1e-5));
    REQUIRE_THAT(wall.vertices[2].y, WithinAbs(-0.75, 1e-5));
    REQUIRE_THAT(wall.vertices[2].z, WithinAbs(0.0, 1e-5));

    const WorldLocatorRecord& camera = world.locators[0];
    REQUIRE(camera.kind == LocatorKind::CameraStart);
    REQUIRE(camera.delay == 5);
    REQUIRE(camera.next == 2);
    REQUIRE(camera.position == Vec3{3.59f, 20.52f, -9.23f});
    REQUIRE(camera.rotation.x == 0.551f);
    REQUIRE(camera.rotation.y == 3.138f);
}

TEST_CASE("the tower's collision triangles unfold into their planes", "[formats][world][assets]") {
    const WorldFile world =
        WorldFile::parse(readFile(test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2")));
    REQUIRE(world.animations.size() == 170);
    REQUIRE(world.animations[2].frameCount == 216); // the magic circle's keys
    // Its seven particle templates and the items it places.
    REQUIRE(world.particles.size() == 7);
    REQUIRE(world.particles[4].id == 'E');
    REQUIRE(world.particles[4].texture == "P_TORCH");
    REQUIRE(world.particles[4].rate[0] == 25.0f);
    REQUIRE(world.itemInfos.size() == 91);
    REQUIRE(world.itemInfos[40].name == "GEMORANGE");
    REQUIRE(world.itemInfos[40].type == 1);
    REQUIRE(world.itemInfos[40].subtype == 15);
    REQUIRE(world.itemInstances.size() == 216);
    REQUIRE(world.itemInstances[49].info == 40);
    REQUIRE(world.itemInstances[49].minPlayers == 1);
    REQUIRE(world.itemInstances[49].position.x == Approx(19.3f).margin(0.05f));
    REQUIRE(world.collision.size() == world.collisionTriangleCount);
    REQUIRE(world.collision.size() > 10000);
    usize floors = 0;
    for (const WorldCollisionTriangle& triangle : world.collision) {
        const Vec3 a = triangle.vertices[1] - triangle.vertices[0];
        const Vec3 b = triangle.vertices[2] - triangle.vertices[0];
        REQUIRE(std::isfinite(a.x + a.y + a.z + b.x + b.y + b.z));
        // Both edges lie in the plane the stored normal describes.
        REQUIRE_THAT(glm::dot(a, triangle.normal), WithinAbs(0.0, 0.01 * (1.0 + glm::length(a))));
        REQUIRE_THAT(glm::dot(b, triangle.normal), WithinAbs(0.0, 0.01 * (1.0 + glm::length(b))));
        floors += triangle.normal.y > 0.99f ? 1 : 0;
    }
    REQUIRE(floors > 1000);
    // Every object's range stays inside the list.
    for (const WorldObjectRecord& object : world.objects) {
        if (object.collisionTriangleCount > 0) {
            REQUIRE(object.collisionTriangleIndex >= 0);
            REQUIRE(static_cast<usize>(object.collisionTriangleIndex) +
                        static_cast<usize>(object.collisionTriangleCount) <=
                    world.collision.size());
        }
    }
}

TEST_CASE("an item record of no type lists the records a container picks among",
          "[formats][world]") {
    std::vector<u8> bytes = sampleWorld(false);
    const usize record = bytes.size() - kItemInstance - kItemInfo;
    put32(bytes, record, static_cast<u32>(ItemInfoRecord::kChoiceList));
    put32(bytes, record + 4, 3); // how many it lists
    put16(bytes, record + 8, 94);
    put16(bytes, record + 10, 98);
    put16(bytes, record + 12, 97);
    put16(bytes, record + 14, 95); // past its count
    const WorldFile world = WorldFile::parse(bytes);
    REQUIRE(world.itemInfos.size() == 1);
    REQUIRE(world.itemInfos[0].type == ItemInfoRecord::kChoiceList);
    REQUIRE(world.itemInfos[0].choices == std::vector<s16>{94, 98, 97});
    // An ordinary record lists nothing, and keeps its sizes.
    REQUIRE(WorldFile::parse(sampleWorld(false)).itemInfos[0].choices.empty());
    // A count beyond what a record can hold is cut to that.
    put32(bytes, record + 4, 4000);
    REQUIRE(WorldFile::parse(bytes).itemInfos[0].choices.size() == ItemInfoRecord::kMostChoices);
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
