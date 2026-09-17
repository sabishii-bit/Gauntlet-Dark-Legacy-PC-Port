#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/TextureSet.h"
#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"

namespace {

using namespace gdl;

std::filesystem::path sampleSet(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "textures");
    writeFile(dir / "textures/000_GLOW_.png", test::kTinyPng);
    writeFile(dir / "textures/001_GLOW_+1.png", test::kTinyPng);
    writeTextFile(dir / "textures.json", R"({
  "source": "/sample/",
  "defs": [
    {"name": "GLOW_", "index": 0, "width": 2, "height": 2},
    {"name": "ALIAS", "index": 1, "width": 2, "height": 2}
  ],
  "bitmaps": [
    {"index": 0, "name": "GLOW_", "file": "textures/000_GLOW_.png", "width": 2, "height": 2,
     "format": 50, "flags": 141, "halfResolution": true, "frames": 2, "clampU": true},
    {"index": 1, "name": "GLOW_+1", "file": "textures/001_GLOW_+1.png", "width": 2, "height": 2,
     "format": 50, "flags": 12, "halfResolution": false, "frames": 0}
  ]
})");
    return dir;
}

TEST_CASE("a texture set reads its manifest and resolves names", "[assets][textures]") {
    TextureSet set;
    REQUIRE(set.load(sampleSet("texture-set")));
    REQUIRE(set.loaded());
    REQUIRE(set.size() == 2);
    REQUIRE(set.find("glow_") == 0U);
    REQUIRE(set.find("GLOW_+1") == 1U);
    REQUIRE(set.find("alias") == 1U);
    REQUIRE_FALSE(set.find("nothing").has_value());

    const TextureSetEntry& glow = set.entry(0);
    REQUIRE(glow.name == "GLOW_");
    REQUIRE(glow.width == 2);
    REQUIRE(glow.flags == 141);
    REQUIRE(glow.frames == 2);
    REQUIRE(glow.halfResolution);
    REQUIRE_FALSE(set.entry(1).halfResolution);

    const Image& image = set.image(1);
    REQUIRE(image.width == 2);
    REQUIRE(image.pixel(0, 0) == Color::rgba(255, 0, 0, 255));
}

TEST_CASE("GPU textures are created once and released on demand", "[assets][textures]") {
    TextureSet set;
    REQUIRE(set.load(sampleSet("texture-set-gpu")));
    test::FakeRenderDevice device;
    const Texture& first = set.texture(device, 0);
    const Texture& again = set.texture(device, 0);
    REQUIRE(&first == &again);
    REQUIRE(first.width() == 2);
    REQUIRE(device.texturesCreated == 1);
    set.texture(device, 1);
    REQUIRE(device.texturesCreated == 2);
    set.releaseTextures();
    set.texture(device, 0);
    REQUIRE(device.texturesCreated == 3);
}

TEST_CASE("clamped tiles are created with edge clamping", "[assets][textures]") {
    TextureSet set;
    REQUIRE(set.load(sampleSet("texture-set-clamp")));
    REQUIRE(set.entry(0).clamp);
    REQUIRE_FALSE(set.entry(1).clamp);
    test::FakeRenderDevice device;
    set.texture(device, 0);
    REQUIRE(device.lastTextureDesc.wrap == TextureWrap::ClampToEdge);
    set.texture(device, 1);
    REQUIRE(device.lastTextureDesc.wrap == TextureWrap::Repeat);
}

TEST_CASE("missing images surface as file errors", "[assets][textures]") {
    const auto dir = sampleSet("texture-set-missing");
    std::filesystem::remove(dir / "textures/001_GLOW_+1.png");
    TextureSet set;
    REQUIRE(set.load(dir));
    REQUIRE_THROWS_AS(set.image(1), FileError);
}

TEST_CASE("a missing or malformed manifest fails to load", "[assets][textures]") {
    TextureSet set;
    REQUIRE_FALSE(set.load(test::scratchDirectory("texture-set-empty")));
    REQUIRE_FALSE(set.loaded());

    const auto dir = test::scratchDirectory("texture-set-bad");
    writeTextFile(dir / "textures.json", R"({ "bitmaps": [ { "name": 3 } ] )");
    REQUIRE_FALSE(set.load(dir));
    REQUIRE(set.size() == 0);
}

TEST_CASE("the unpacked title set names the backdrop tiles", "[assets][textures][unpacked]") {
    const auto dir = test::unpackedOrSkip("TITLE/textures.json").parent_path();
    TextureSet set;
    REQUIRE(set.load(dir));
    REQUIRE(set.size() == 16);
    REQUIRE(set.find("TITLE00") == 11U);
    REQUIRE(set.entry(0).frames == 10);
    REQUIRE(set.entry(0).halfResolution);
}

} // namespace
