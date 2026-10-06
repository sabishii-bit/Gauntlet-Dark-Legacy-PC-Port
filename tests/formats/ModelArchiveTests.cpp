#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/ModelArchive.h"

namespace {

using namespace gdl;
using namespace gdl::formats;
using test::ByteWriter;

/** A v13 archive with one object (two sub-objects), two bitmaps and names for both. */
std::vector<u8> sampleArchive() {
    constexpr u32 kObjectsAt = 160;
    constexpr u32 kBitmapsAt = kObjectsAt + 64;
    constexpr u32 kObjectDefsAt = kBitmapsAt + 128;
    constexpr u32 kBitmapDefsAt = kObjectDefsAt + 24;
    constexpr u32 kSubObjectsAt = kBitmapDefsAt + 72;
    constexpr u32 kModelsAt = kSubObjectsAt + 16;

    ByteWriter w;
    w.putText("/disk/sample/").putZeros(32 - 13);
    w.putZeros(32);
    w.putU32(ModelArchive::kVersion13);
    w.putU32(1).putU32(2).putU32(1).putU32(2);
    w.putU32(kObjectsAt).putU32(kBitmapsAt).putU32(kObjectDefsAt).putU32(kBitmapDefsAt);
    w.putU32(kSubObjectsAt).putU32(kSubObjectsAt + 8).putU32(0).putU32(0).putU32(0).putU32(0);
    w.putU16(0).putU16(0).putU32(0).putZeros(28);
    REQUIRE(w.size() == kObjectsAt);

    // object: inv_rad, bnd_rad, flags, sub count, sub0 (qwc tex lm lodk), subs ptr, models ptr,
    // verts, tris, id, obj_def, pad
    w.putU32(0).putU32(0x40000000).putU32(0x0A).putU32(2);
    w.putU16(2).putU16(1).putU16(0).putU16(static_cast<u16>(-5));
    w.putU32(kSubObjectsAt).putU32(kModelsAt).putU32(6).putU32(4).putU32(7).putU32(0).putZeros(16);
    REQUIRE(w.size() == kBitmapsAt);

    for (s32 i = 0; i < 2; ++i) {
        w.putU8(i == 0 ? 50 : 0).putU8(static_cast<u8>(-64)).putU8(0).putU8(1);
        w.putU16(3).putU16(3).putU16(i == 0 ? 0x008D : 0x000C).putU16(0);
        w.putU32(i == 0 ? 0 : 0x1200).putU16(0).putU16(0).putU16(i == 0 ? 4 : 0);
        w.putU16(8).putU16(8).putU16(64).putU32(0).putZeros(32);
    }
    REQUIRE(w.size() == kObjectDefsAt);

    w.putText("THING").putZeros(11).putU32(0x40000000).putU16(0).putU16(0);
    REQUIRE(w.size() == kBitmapDefsAt);
    w.putText("Glow_").putZeros(25).putU16(0).putU16(8).putU16(8);
    w.putText("plain").putZeros(25).putU16(1).putU16(8).putU16(8);
    REQUIRE(w.size() == kSubObjectsAt);

    w.putU16(3).putU16(0).putU16(1).putU16(0).putZeros(8);
    REQUIRE(w.size() == kModelsAt);
    // two models: 1 and 2 quadwords of payload each, header quadword first
    w.putU16(1).putZeros(14);
    for (u8 b = 0; b < 16; ++b) {
        w.putU8(static_cast<u8>(0xA0 + b));
    }
    w.putU16(2).putZeros(14).putZeros(32);
    return w.bytes();
}

TEST_CASE("a synthetic archive parses into records and names", "[formats][archive]") {
    const ModelArchive archive = ModelArchive::parse(sampleArchive());
    REQUIRE(archive.sourceDirectory() == "/disk/sample/");
    REQUIRE(archive.version() == ModelArchive::kVersion13);
    REQUIRE(archive.objects().size() == 1);
    REQUIRE(archive.bitmaps().size() == 2);
    REQUIRE(archive.objectDefs().size() == 1);
    REQUIRE(archive.bitmapDefs().size() == 2);

    const ArchiveObject& object = archive.objects()[0];
    REQUIRE(object.boundingRadius == 2.0f);
    REQUIRE(object.flags == 0x0A);
    REQUIRE(object.vertexCount == 6);
    REQUIRE(object.triangleCount == 4);
    REQUIRE(object.id == 7);
    REQUIRE(object.subObjects.size() == 2);
    REQUIRE(object.subObjects[0].textureIndex == 1);
    REQUIRE(object.subObjects[0].lodK == -5);
    REQUIRE(object.subObjects[0].geometry.size() == 32);
    REQUIRE(object.subObjects[0].geometry[16] == 0xA0);
    REQUIRE(object.subObjects[1].quadwordCount == 3);
    REQUIRE(object.subObjects[1].lightmapIndex == 1);
    REQUIRE(object.subObjects[1].geometry.size() == 48);

    REQUIRE(archive.bitmaps()[0].format == 50);
    REQUIRE(archive.bitmaps()[0].frameCount == 4);
    REQUIRE(archive.bitmaps()[0].flags == 0x008D);
    REQUIRE(archive.bitmaps()[1].dataOffset == 0x1200);
    REQUIRE(archive.bitmaps()[1].lodK == -64);

    REQUIRE(archive.findBitmap("glow_") == 0U);
    REQUIRE(archive.findBitmap("  PLAIN ") == 1U);
    REQUIRE_FALSE(archive.findBitmap("missing").has_value());
    REQUIRE(archive.findObject("thing") == 0U);
    REQUIRE_FALSE(archive.findObject("other").has_value());
}

TEST_CASE("damaged archives are rejected", "[formats][archive]") {
    REQUIRE_THROWS_AS(ModelArchive::parse(std::vector<u8>(100, 0)), FormatError);
    std::vector<u8> bad = sampleArchive();
    bad[64] = 0x01; // wrong version
    REQUIRE_THROWS_AS(ModelArchive::parse(bad), FormatError);
    std::vector<u8> truncated = sampleArchive();
    truncated.resize(200);
    REQUIRE_THROWS_AS(ModelArchive::parse(truncated), FormatError);
}

TEST_CASE("v12 continuation records are six bytes but embedded records retain LOD",
          "[formats][archive][asset-conformance]") {
    for (const u32 version : {ModelArchive::kVersion12, ModelArchive::kVersion13}) {
        CAPTURE(version);
        constexpr u32 kObjectsAt = 160;
        constexpr u32 kSubsAt = 224;
        constexpr u32 kGeometryAt = 240;
        ByteWriter bytes;
        bytes.putZeros(64).putU32(version).putU32(1).putZeros(12);
        bytes.putU32(kObjectsAt).putZeros(12).putZeros(kObjectsAt - 100);
        bytes.putZeros(12).putU32(3);
        bytes.putU16(1).putU16(3).putU16(0).putU16(9);
        bytes.putU32(kSubsAt).putU32(kGeometryAt).putZeros(32);
        REQUIRE(bytes.size() == kSubsAt);
        for (const u16 texture : {u16{5}, u16{7}}) {
            bytes.putU16(1).putU16(texture).putU16(2);
            if (version == ModelArchive::kVersion13) {
                bytes.putU16(4);
            }
        }
        bytes.putZeros(kGeometryAt - bytes.size()).putZeros(48);
        const auto parsed = ModelArchive::parse(bytes.bytes());
        const auto& subs = parsed.objects()[0].subObjects;
        REQUIRE(subs.size() == 3);
        CHECK(subs[0].lodK == 9);
        CHECK(subs[1].textureIndex == 5);
        CHECK(subs[2].textureIndex == 7);
        CHECK(subs[1].lightmapIndex == 2);
        CHECK(subs[2].lightmapIndex == 2);
        CHECK(subs[1].lodK == (version == ModelArchive::kVersion13 ? 4 : 0));
        CHECK(subs[2].geometry.size() == 16);
    }
}

TEST_CASE("native L2 podium and Sumner decode every v12 mesh part",
          "[formats][archive][asset-conformance][assets]") {
    const auto file = test::assetOrSkip("LEVELS/levelL2/objects.ngc");
    ModelSet models;
    REQUIRE(models.load(file.parent_path()));
    const auto parsed = ModelArchive::parse(readFile(file));
    REQUIRE(parsed.version() == ModelArchive::kVersion12);
    for (const auto* name : {"L2NSPODIUM", "L2NSSUMNER"}) {
        CAPTURE(name);
        const auto index = models.find(name);
        REQUIRE(index);
        const auto& mesh = models.mesh(*index);
        REQUIRE_FALSE(mesh.vertices.empty());
        REQUIRE(mesh.parts.size() == parsed.objects()[*index].subObjects.size());
        for (const auto& part : mesh.parts) {
            CHECK(part.texture < parsed.bitmaps().size());
            CHECK(part.lightmap < parsed.bitmaps().size());
        }
    }
}

TEST_CASE("the title archive lists its backdrop and glow textures", "[formats][archive][assets]") {
    const auto file = test::assetOrSkip("TITLE/objects.ngc");
    const ModelArchive archive = ModelArchive::parse(readFile(file));
    REQUIRE(archive.objects().size() == 1);
    REQUIRE(archive.bitmaps().size() == 16);
    REQUIRE(archive.bitmapDefs().size() == 6);
    REQUIRE(archive.findBitmap("TITLE00") == 11U);
    REQUIRE(archive.findBitmap("title03") == 14U);
    REQUIRE(archive.findBitmap("GLOWCROP_") == 0U);
    REQUIRE(archive.bitmaps()[0].frameCount == 10);
    REQUIRE((archive.bitmaps()[0].flags & bitmap_flags::kHalfResolution) != 0);
    REQUIRE(archive.bitmaps()[11].width == 256);
    REQUIRE(archive.bitmaps()[13].height == 128);
    REQUIRE(archive.objects()[0].subObjects.size() == 1);
    REQUIRE(archive.objects()[0].subObjects[0].geometry.size() == 80);
}

TEST_CASE("the static archive names its objects", "[formats][archive][assets]") {
    const auto file = test::assetOrSkip("STATIC/objects.ngc");
    const ModelArchive archive = ModelArchive::parse(readFile(file));
    REQUIRE(archive.objects().size() == 7);
    REQUIRE(archive.findObject("COLARC") == 2U);
    REQUIRE(archive.findBitmap("FONT32") == 33U);
    REQUIRE(archive.bitmaps()[33].format == bitmap_format::kAlpha4);
}

} // namespace
