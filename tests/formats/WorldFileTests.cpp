#include <algorithm>
#include <bit>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/PortableMath.h"

#include "TestSupport.h"
#include "formats/WorldFile.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using Catch::Approx;
using Catch::Matchers::WithinAbs;
using test::setF32LE;
using test::setText;
using test::setU16LE;
using test::setU32LE;

constexpr usize kHeader = 96;
constexpr usize kExtended = 24;
constexpr usize kObject = 60;
constexpr usize kLocator = 28;
constexpr usize kTriangle = 40;

/** The fixed name fields of an object or item record, and of a particle template's texture. */
constexpr usize kNameField = 16;
constexpr usize kTextureField = 32;

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
    setU32LE(bytes, 0, 2);
    setU32LE(bytes, 4, static_cast<u32>(objects));
    setU32LE(bytes, 8, 1); // collision triangles
    setU32LE(bytes, 12, static_cast<u32>(triangles));
    setF32LE(bytes, 36, -61.1f);
    setF32LE(bytes, 40, -50.9f);
    setF32LE(bytes, 44, -50.0f);
    setF32LE(bytes, 48, 249.9f);
    setF32LE(bytes, 52, 17.9f);
    setF32LE(bytes, 56, 50.0f);
    setF32LE(bytes, 60, 8.0f);
    setU32LE(bytes, 64, 39);
    setU32LE(bytes, 68, 13);
    // One kind of item, an orange crystal, placed once for a party of one.
    setU32LE(bytes, 72, 1);
    setU32LE(bytes, 76, static_cast<u32>(itemInfo));
    setU32LE(bytes, 80, 1);
    setU32LE(bytes, 84, static_cast<u32>(itemInstance));
    setU32LE(bytes, 88, 1);
    setU32LE(bytes, 92, static_cast<u32>(locators));
    setU32LE(bytes, itemInfo, 1);
    setU32LE(bytes, itemInfo + 4, 15);
    setU16LE(bytes, itemInfo + 8, 1);
    setF32LE(bytes, itemInfo + 12, 0.1f);
    setF32LE(bytes, itemInfo + 16, 2.0f);
    setText(bytes, itemInfo + 0x28, kNameField, "GEMORANGE");
    setU16LE(bytes, itemInfo + 0x40, 4);
    setU16LE(bytes, itemInfo + 0x42, static_cast<u16>(-1));
    setU16LE(bytes, itemInfo + 0x46, 16);
    setU16LE(bytes, itemInstance, 0);
    bytes[itemInstance + 2] = 1;
    setF32LE(bytes, itemInstance + 0x18, 19.3f);
    setF32LE(bytes, itemInstance + 0x1C, -2.0f);
    setF32LE(bytes, itemInstance + 0x20, -57.5f);
    setF32LE(bytes, itemInstance + 0x28, 1.5f);
    bytes[itemInstance + 0x30] = 7;
    if (extended) {
        setU32LE(bytes, kHeader, 0xF00BAB02);
        setU32LE(bytes, kHeader + 4, static_cast<u32>(keys));
        setU32LE(bytes, kHeader + 8, 1);
        setU32LE(bytes, kHeader + 12, static_cast<u32>(animation));
        setU32LE(bytes, kHeader + 16, 1);
        setU32LE(bytes, kHeader + 20, static_cast<u32>(particle));
        setU32LE(bytes, keys + 12, 36); // blocks follow the header and its one entry
        setU32LE(bytes, keys + 20, 1);
        setU32LE(bytes, keys + 24, 1);
        setU16LE(bytes, keys + 28, 0x0004); // rotation z, one float a key
        setU16LE(bytes, keys + 30, 1);
        setU32LE(bytes, keys + 36, 0x9); // keys at frames 0 and 3
        setF32LE(bytes, keys + 40, 0.0f);
        setF32LE(bytes, keys + 44, 1.5f);
        setU16LE(bytes, animation, 1); // the mesh
        setU16LE(bytes, animation + 2, 4);
        setU16LE(bytes, animation + 6, 0x101);
        setU32LE(bytes, animation + 12, static_cast<u32>(keys + 28));
        setU32LE(bytes, particle, 0x101);
        setU16LE(bytes, particle + 4, 5);
        bytes[particle + 6] = static_cast<u8>('E');
        setU32LE(bytes, particle + 8, 0x288);
        setU32LE(bytes, particle + 12, 0x288);
        setU32LE(bytes, particle + 16, 0x56BE1);
        setF32LE(bytes, particle + 0x28, 0.2f);
        setF32LE(bytes, particle + 0x2C, 0.22f);
        setF32LE(bytes, particle + 0x38, 80.0f);
        setText(bytes, particle + 0x40, kTextureField, "P_TORCH");
        setF32LE(bytes, particle + 0x64, 1.0f);
        setF32LE(bytes, particle + 0x6C, 0.1f);
        setF32LE(bytes, particle + 0x70, 0.3f);
        setF32LE(bytes, particle + 0x74, 0.1f);
        for (usize i = 0; i < 4; ++i) {
            setF32LE(bytes, particle + 0x78 + i * 4, 25.0f);
        }
        setF32LE(bytes, particle + 0x8C, -0.38f);
        setF32LE(bytes, particle + 0x94, 4.0f);
        setU32LE(bytes, particle + 0x9C, 0x00FFFFFF);
        setU32LE(bytes, particle + 0xA0, 0x00FFFFFF);
        setF32LE(bytes, particle + 0xA8, 2.8f);
        setF32LE(bytes, particle + 0xAC, 2.0f);
        setF32LE(bytes, particle + 0xB0, 2.0f);
        setF32LE(bytes, particle + 0xB4, 0.1f);
    }
    // object 0: a group whose child is object 1
    setText(bytes, objects, kNameField, "T1FLOOR1");
    setU32LE(bytes, objects + 16, 0x4);
    setF32LE(bytes, objects + 28, 1.0f);
    setF32LE(bytes, objects + 32, 2.0f);
    setF32LE(bytes, objects + 36, 3.0f);
    setU16LE(bytes, objects + 44, 0xFFFF); // next -1
    setU16LE(bytes, objects + 46, 1);      // child
    // object 1: the mesh
    const usize second = objects + kObject;
    setText(bytes, second, kNameField, "T1#0");
    setU32LE(bytes, second + 16, 0x6);
    setU32LE(bytes, second + 24, 0x8000);
    setF32LE(bytes, second + 28, 0.5f);
    setU16LE(bytes, second + 44, 0xFFFF);
    setU16LE(bytes, second + 46, 0xFFFF);
    setF32LE(bytes, second + 48, 70.7f);
    bytes[second + 53] = 1; // no collision
    setU16LE(bytes, second + 54, 1);
    setU32LE(bytes, second + 56, 0);
    // locator: camera_start
    bytes[locators] = 1;
    bytes[locators + 1] = 5;
    setU16LE(bytes, locators + 2, 2);
    setF32LE(bytes, locators + 4, 3.59f);
    setF32LE(bytes, locators + 8, 20.52f);
    setF32LE(bytes, locators + 12, -9.23f);
    setF32LE(bytes, locators + 16, 0.551f);
    setF32LE(bytes, locators + 20, 3.138f);
    // A wall facing +z with its corner at (0, 1, 0), 1 unit wide and 1.75 tall (1/64 units).
    setU16LE(bytes, triangles, static_cast<u16>(-48));
    setU16LE(bytes, triangles + 2, 64);
    setF32LE(bytes, triangles + 4, 1.0f);
    setF32LE(bytes, triangles + 16, 1.0f); // normal z
    setF32LE(bytes, triangles + 24, 1.0f); // corner y
    setU16LE(bytes, triangles + 32, 64);
    setU16LE(bytes, triangles + 34, 0);
    setU16LE(bytes, triangles + 36, 0);
    setU16LE(bytes, triangles + 38, static_cast<u16>(-112));
    return bytes;
}

TEST_CASE("collision corners unfold with the portable functions on every platform",
          "[formats][world][determinism]") {
    // Normals from a fixed integer stream, unit offsets on a zero base: every bit of each
    // sine and cosine lands in a corner, so a platform's own trigonometry would show.
    constexpr usize kTriangles = 2048;
    const usize triangles = kHeader + kExtended; // after an extended header naming nothing
    std::vector<u8> bytes(triangles + kTriangles * kTriangle, 0);
    setU32LE(bytes, 8, static_cast<u32>(kTriangles));
    setU32LE(bytes, 12, static_cast<u32>(triangles));
    setU32LE(bytes, kHeader, 0xF00BAB02);
    std::vector<Vec3> normals;
    u32 state = 12345;
    const auto component = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<f32>(static_cast<s32>(state >> 8) - (1 << 23)) /
               static_cast<f32>(1 << 23);
    };
    while (normals.size() < kTriangles) {
        const Vec3 raw{component(), component(), component()};
        if (glm::dot(raw, raw) < 0.01f) {
            continue;
        }
        normals.push_back(glm::normalize(raw));
    }
    for (usize i = 0; i < kTriangles; ++i) {
        const usize at = triangles + i * kTriangle;
        setF32LE(bytes, at + 8, normals[i].x);
        setF32LE(bytes, at + 12, normals[i].y);
        setF32LE(bytes, at + 16, normals[i].z);
        setU16LE(bytes, at + 32, 64); // (1, 0)
        setU16LE(bytes, at + 38, 64); // (0, 1)
    }
    const WorldFile world = WorldFile::parse(bytes);
    REQUIRE(world.collision.size() == kTriangles);
    for (usize i = 0; i < kTriangles; ++i) {
        const Vec3& n = normals[i];
        const f32 tilt = portable::acos(std::clamp(n.y, -1.0f, 1.0f));
        const f32 turn = n.x == 0.0f && n.z == 0.0f ? 0.0f : portable::atan2(-n.x, -n.z);
        const f32 sinTilt = portable::sin(tilt);
        const f32 cosTilt = portable::cos(tilt);
        const f32 sinTurn = portable::sin(turn);
        const f32 cosTurn = portable::cos(turn);
        // The decoder's own sums, over a zero corner, so that even a zero's sign agrees.
        const auto unfold = [&](f32 x, f32 z) {
            return Vec3{0.0f} + Vec3{x * cosTurn + z * cosTilt * sinTurn, z * sinTilt,
                                     -x * sinTurn + z * cosTilt * cosTurn};
        };
        const Vec3 first = unfold(1.0f, 0.0f);
        const Vec3 second = unfold(0.0f, 1.0f);
        CAPTURE(i);
        for (glm::length_t axis = 0; axis < 3; ++axis) {
            CHECK(std::bit_cast<u32>(world.collision[i].vertices[1][axis]) ==
                  std::bit_cast<u32>(first[axis]));
            CHECK(std::bit_cast<u32>(world.collision[i].vertices[2][axis]) ==
                  std::bit_cast<u32>(second[axis]));
        }
    }
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
    setU32LE(bytes, record, static_cast<u32>(ItemInfoRecord::kChoiceList));
    setU32LE(bytes, record + 4, 3); // how many it lists
    setU16LE(bytes, record + 8, 94);
    setU16LE(bytes, record + 10, 98);
    setU16LE(bytes, record + 12, 97);
    setU16LE(bytes, record + 14, 95); // past its count
    const WorldFile world = WorldFile::parse(bytes);
    REQUIRE(world.itemInfos.size() == 1);
    REQUIRE(world.itemInfos[0].type == ItemInfoRecord::kChoiceList);
    REQUIRE(world.itemInfos[0].choices == std::vector<s16>{94, 98, 97});
    // An ordinary record lists nothing, and keeps its sizes.
    REQUIRE(WorldFile::parse(sampleWorld(false)).itemInfos[0].choices.empty());
    // A count beyond what a record can hold is cut to that.
    setU32LE(bytes, record + 4, 4000);
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
    setU32LE(bytes, kHeader, 0x12345678);
    REQUIRE_THROWS_AS(WorldFile::parse(bytes), FormatError);

    bytes = sampleWorld(true);
    setU32LE(bytes, 0, 1000);
    REQUIRE_THROWS_AS(WorldFile::parse(bytes), FormatError);

    bytes = sampleWorld(true);
    setU32LE(bytes, 92, 100000);
    REQUIRE_THROWS_AS(WorldFile::parse(bytes), FormatError);
}

TEST_CASE("nonempty world effects cannot silently disappear behind invalid table offsets",
          "[formats][world][asset-conformance]") {
    for (const usize field : {kHeader + 4, kHeader + 12, kHeader + 20}) {
        for (const u32 target : {0U, 96U, 119U, 0xFFFFFFF0U}) {
            CAPTURE(field, target);
            auto bytes = sampleWorld(true);
            setU32LE(bytes, field, target);
            CHECK_THROWS_AS(WorldFile::parse(bytes), FormatError);
        }
    }
    auto empty = sampleWorld(true);
    setU32LE(empty, kHeader + 8, 0);
    setU32LE(empty, kHeader + 16, 0);
    setU32LE(empty, kHeader + 4, 0);
    setU32LE(empty, kHeader + 12, 0);
    setU32LE(empty, kHeader + 20, 0);
    const auto parsed = WorldFile::parse(empty);
    CHECK(parsed.animations.empty());
    CHECK(parsed.particles.empty());
}

} // namespace
