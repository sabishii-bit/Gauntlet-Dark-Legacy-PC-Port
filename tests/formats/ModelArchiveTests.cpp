#include <vector>

#include <catch2/catch_test_macros.hpp>

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
