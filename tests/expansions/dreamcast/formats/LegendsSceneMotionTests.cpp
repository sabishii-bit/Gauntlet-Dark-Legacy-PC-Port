#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsSceneMotion.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

void put16(std::vector<u8>& data, usize at, u16 value) {
    data.at(at) = static_cast<u8>(value);
    data.at(at + 1) = static_cast<u8>(value >> 8);
}
void put32(std::vector<u8>& data, usize at, u32 value) {
    put16(data, at, static_cast<u16>(value));
    put16(data, at + 2, static_cast<u16>(value >> 16));
}
void putFloat(std::vector<u8>& data, usize at, f32 value) {
    put32(data, at, std::bit_cast<u32>(value));
}

std::vector<u8> world() {
    std::vector<u8> bytes(364);
    put32(bytes, 0, 3);
    put32(bytes, 4, 120);
    put32(bytes, 96, 0xf00bab02);
    put32(bytes, 100, 300);
    put32(bytes, 104, 1);
    put32(bytes, 108, 348);
    for (const usize at : {120U, 180U, 240U}) {
        put16(bytes, at + 44, 0xffff);
        put16(bytes, at + 46, 0xffff);
    }
    put16(bytes, 120 + 46, 1);
    put16(bytes, 180 + 44, 2);
    putFloat(bytes, 120 + 28, 10.0f);
    putFloat(bytes, 240 + 28, 2.0f);
    put32(bytes, 300 + 12, 36);
    put32(bytes, 300 + 20, 1);
    put32(bytes, 300 + 24, 1);
    put16(bytes, 328, NodeTrack::kRotationY);
    put16(bytes, 330, 1);
    put32(bytes, 336, 0x9);
    putFloat(bytes, 340, 0.0f);
    putFloat(bytes, 344, 1.5f);
    put16(bytes, 348, 2);
    put16(bytes, 350, 4);
    put16(bytes, 354, 0x101);
    put32(bytes, 360, 328);
    return bytes;
}

std::vector<u8> animation() {
    std::vector<u8> bytes(104);
    put32(bytes, 8, 1);
    put32(bytes, 12, 16);
    put16(bytes, 16, 0xffff);
    constexpr std::string_view kName = "WATER";
    std::copy(kName.begin(), kName.end(), bytes.begin() + 20);
    put32(bytes, 16 + 0x44, 3);
    put32(bytes, 16 + 0x48, static_cast<u32>(TextureAnimation::kScrollV));
    put16(bytes, 16 + 0x4c, 120);
    put32(bytes, 16 + 0x50, 4);
    return bytes;
}
} // namespace

TEST_CASE("Dreamcast motion retains native hierarchy keys and texture clocks",
          "[formats][legends][motion]") {
    const auto data = LegendsSceneMotion::parseDreamcast(world(), animation());
    REQUIRE(data.parents == std::vector<s32>{-1, 0, 0});
    REQUIRE(data.objects.size() == 3);
    CHECK(data.objects[0].position.x == 10.0f);
    CHECK(data.objects[2].position.x == 2.0f);
    REQUIRE(data.worldAnimations.size() == 1);
    const auto& track = data.worldAnimations[0];
    CHECK(track.objectIndex == 2);
    CHECK(track.frameCount == 4);
    CHECK(track.state == 0x101);
    CHECK(track.track.frames == std::vector<u16>{0, 3});
    CHECK(track.track.values == std::vector<f32>{0.0f, 1.5f});
    REQUIRE(data.archive.textureAnimations.size() == 1);
    const auto& texture = data.archive.textureAnimations[0];
    CHECK(texture.texture == 3);
    CHECK(texture.source == TextureAnimation::kScrollV);
    CHECK(texture.frames == 120);
    CHECK(texture.rate == 4);
    CHECK(texture.flag == -1);
    CHECK(data.worldParticles.empty());
    CHECK(data.archive.particles.empty());
}

TEST_CASE("Dreamcast motion rejects corrupt links and keyframes", "[formats][legends][motion]") {
    auto bytes = world();
    SECTION("old backup revision") {
        put32(bytes, 96, 0xf00bab01);
    }
    SECTION("truncated world") {
        bytes.resize(119);
    }
    SECTION("child out of bounds") {
        put16(bytes, 166, 50);
    }
    SECTION("sibling out of bounds") {
        put16(bytes, 224, 50);
    }
    SECTION("sibling loop") {
        put16(bytes, 284, 1);
    }
    SECTION("root sibling loop") {
        put16(bytes, 166, 0xffff);
        put16(bytes, 164, 1);
        put16(bytes, 284, 0);
    }
    SECTION("ancestor loop") {
        put16(bytes, 286, 0);
    }
    SECTION("duplicate child") {
        put16(bytes, 226, 2);
    }
    SECTION("bad target") {
        put16(bytes, 348, 3);
    }
    SECTION("nonfinite origin") {
        putFloat(bytes, 148, std::numeric_limits<f32>::infinity());
    }
    SECTION("nonfinite key") {
        putFloat(bytes, 344, std::numeric_limits<f32>::quiet_NaN());
    }
    CHECK_THROWS_AS(LegendsSceneMotion::parseDreamcast(bytes), FormatError);
}

TEST_CASE("Dreamcast zero-frame initial-only motion retains its static pose",
          "[formats][legends][motion]") {
    auto bytes = world();
    put16(bytes, 328, NodeTrack::kRotationY | 0x4000);
    put16(bytes, 350, 0);
    putFloat(bytes, 336, 1.5f);
    const auto data = LegendsSceneMotion::parseDreamcast(bytes);
    REQUIRE(data.worldAnimations.size() == 1);
    CHECK(data.worldAnimations[0].frameCount == 0);
    CHECK(data.worldAnimations[0].track.frames == std::vector<u16>{0});
    CHECK(data.worldAnimations[0].track.values == std::vector<f32>{1.5f});
}

TEST_CASE("Dreamcast animation refuses foreign revisions and truncated tables",
          "[formats][legends][motion]") {
    auto bytes = animation();
    SECTION("foreign version") {
        put16(bytes, 2, 8);
    }
    SECTION("truncated header") {
        bytes.resize(15);
    }
    SECTION("truncated texture row") {
        bytes.pop_back();
    }
    SECTION("invalid texture table") {
        put32(bytes, 12, 0xfffffff0);
    }
    CHECK_THROWS_AS(parseDreamcastAnimation(bytes), FormatError);
}

TEST_CASE("Dreamcast native scene motion uses shared readers across every canonical level",
          "[formats][legends][motion][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::exists(root)) {
        SKIP("Native Dreamcast extraction is not installed");
    }
    usize count = 0;
    usize tracks = 0;
    usize textures = 0;
    for (const auto& folder : std::filesystem::directory_iterator(root / "levels")) {
        if (!folder.is_directory()) {
            continue;
        }
        const auto path = folder.path() / "worlds.rom";
        if (!std::filesystem::exists(path) || std::filesystem::file_size(path) == 0) {
            continue;
        }
        INFO(folder.path().string());
        const auto motion = LegendsSceneMotion::parseDreamcast(
            readFile(path), readFile(folder.path() / "anim.rom"));
        REQUIRE_FALSE(motion.objects.empty());
        CHECK(motion.parents.size() == motion.objects.size());
        CHECK(motion.worldParticles.empty());
        CHECK(motion.archive.particles.empty());
        CHECK(motion.archive.trees.empty());
        tracks += motion.worldAnimations.size();
        textures += motion.archive.textureAnimations.size();
        ++count;
    }
    CHECK(count == 41);
    CHECK(tracks == 1117);
    CHECK(textures == 363);
}

TEST_CASE("Dreamcast item and effect archive trees decode without format conversion",
          "[formats][legends][motion][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::exists(root)) {
        SKIP("Native Dreamcast extraction is not installed");
    }
    usize files = 0;
    usize trees = 0;
    usize tracks = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().filename() != "anim.rom") {
            continue;
        }
        INFO(entry.path().string());
        const auto data = parseDreamcastAnimation(readFile(entry.path()));
        CHECK(data.particles.empty());
        trees += data.trees.size();
        for (const auto& tree : data.trees) {
            for (const auto& sequence : tree.sequences) {
                tracks += sequence.tracks.size();
            }
        }
        ++files;
    }
    CHECK(files == 290);
    CHECK(trees == 1503);
    CHECK(tracks > 10000);
}
