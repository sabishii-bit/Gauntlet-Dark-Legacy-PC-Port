#include <filesystem>
#include <map>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Ambient.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8U);
    bytes.at(at + 1) = static_cast<u8>(value);
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value >> 16U));
    put16(bytes, at + 2, static_cast<u16>(value));
}
std::vector<u8> fixture() {
    std::vector<u8> bytes(116);
    put32(bytes, 28, 92);
    put16(bytes, 52, 2);
    put16(bytes, 92, static_cast<u16>(-170));
    put16(bytes, 94, 685);
    put16(bytes, 96, static_cast<u16>(-359));
    bytes[98] = 45;
    bytes[99] = 1;
    put16(bytes, 100, 7);
    put16(bytes, 102, 36);
    put16(bytes, 104, 32767);
    put16(bytes, 106, 0x8000);
    put16(bytes, 108, 1);
    bytes[110] = 9;
    bytes[111] = 4;
    put16(bytes, 112, 0xffff);
    put16(bytes, 114, 126);
    return bytes;
}
} // namespace

TEST_CASE("N64 ambient records preserve native positions cues and unscaled radii",
          "[legends][audio][native-ambient][formats]") {
    const auto decoded = decodeLegendsN64Ambient(fixture());
    REQUIRE(decoded.size() == 2);
    CHECK(decoded[0].nativeIndex == 0);
    CHECK(decoded[0].position == Vec3{-42.5f, 171.25f, -89.75f});
    CHECK(decoded[0].token == 7);
    CHECK(decoded[0].radius == 36);
    CHECK(decoded[0].kind == 45);
    CHECK(decoded[0].subtype == 1);
    CHECK(decoded[1].nativeIndex == 1);
    CHECK(decoded[1].position == Vec3{8191.75f, -8192, .25f});
    CHECK(decoded[1].token == 0xffff);
    CHECK(decoded[1].radius == 126);
    // The native sound updater does not gate on these preserved metadata bytes.
    CHECK(decoded[1].kind == 9);
    CHECK(decoded[1].subtype == 4);
    CHECK(decodeLegendsN64Ambient({}).empty());
    CHECK(decodeLegendsN64Ambient(std::vector<u8>(92)).empty());
}

TEST_CASE("N64 ambient decoder rejects corrupt sections before exposing emitters",
          "[legends][audio][native-ambient][formats]") {
    auto bytes = fixture();
    SECTION("short header") {
        bytes.resize(91);
    }
    SECTION("truncated record") {
        bytes.pop_back();
    }
    SECTION("record points into header") {
        put32(bytes, 28, 52);
    }
    SECTION("record overlaps native runtime pointer slots") {
        put32(bytes, 28, 56);
    }
    SECTION("record points outside file") {
        put32(bytes, 28, 0xfffffffc);
    }
    SECTION("unbounded count") {
        put16(bytes, 52, 4097);
    }
    SECTION("zero radius") {
        put16(bytes, 102, 0);
    }
    SECTION("negative radius") {
        put16(bytes, 102, 0x8000);
    }
    CHECK_THROWS_AS(decodeLegendsN64Ambient(bytes), FormatError);
}

TEST_CASE("Installed N64 ambient corpus retains all authored emitter locations",
          "[legends][audio][native-ambient][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted/levels";
    if (!std::filesystem::is_regular_file(root / "mount1/ipr.rom")) {
        SKIP("Native N64 extraction is not installed");
    }
    const std::map<std::string, usize> expected{
        {"castle3", 2}, {"ice3", 9},   {"ice4", 6},  {"mount1", 2}, {"mount3", 3},
        {"mount4", 2},  {"mount5", 1}, {"town2", 1}, {"town3", 3},  {"town4", 1}};
    std::map<std::string, usize> actual;
    usize files = 0;
    usize emitters = 0;
    for (const auto& directory : std::filesystem::directory_iterator(root)) {
        const auto path = directory.path() / "ipr.rom";
        if (!directory.is_directory() || !std::filesystem::is_regular_file(path)) {
            continue;
        }
        ++files;
        const auto decoded = decodeLegendsN64Ambient(readFile(path));
        if (!decoded.empty()) {
            actual.emplace(directory.path().filename().string(), decoded.size());
        }
        emitters += decoded.size();
        for (usize i = 0; i < decoded.size(); ++i) {
            CAPTURE(path, i);
            CHECK(decoded[i].nativeIndex == i);
            CHECK(decoded[i].kind == 45);
            CHECK(decoded[i].subtype == 1);
            CHECK(decoded[i].radius > 0);
            CHECK((decoded[i].token == 0 || decoded[i].token == 7 || decoded[i].token == 276 ||
                   decoded[i].token == 277));
        }
    }
    CHECK(files == 35);
    CHECK(emitters == 30);
    CHECK(actual == expected);
    const auto mountain = decodeLegendsN64Ambient(readFile(root / "mount1/ipr.rom"));
    REQUIRE(mountain.size() == 2);
    CHECK(mountain[0].position == Vec3{-42.5f, 171.25f, -89.75f});
    CHECK(mountain[0].radius == 36);
    CHECK(mountain[1].position == Vec3{-32.75f, 38, 66});
    CHECK(mountain[1].radius == 126);
    CHECK(mountain[0].token == 7);
    CHECK(mountain[1].token == 7);
}
