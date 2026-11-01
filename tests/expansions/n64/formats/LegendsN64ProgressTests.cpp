#include <array>
#include <bit>
#include <filesystem>
#include <limits>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Progress.h"

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
void real(std::vector<u8>& bytes, usize at, f32 value) {
    put32(bytes, at, std::bit_cast<u32>(value));
}
struct Fixture {
    std::vector<u8> placements = std::vector<u8>(116);
    std::vector<u8> templates = std::vector<u8>(100);
    Fixture() {
        put32(placements, 12, 92);
        put16(placements, 42, 1);
        put16(placements, 92, 40);
        put16(placements, 94, 80);
        put16(placements, 96, static_cast<u16>(-120));
        put16(placements, 98, static_cast<u16>(-16));
        put16(placements, 100, 0x2601);
        placements[103] = 2;
        put32(placements, 104, 6);
        real(placements, 108, 2);
        put32(templates, 0, 1);
        put16(templates, 4, 0x2601);
        put16(templates, 6, 0x1009);
        put32(templates, 8, 0x02000000);
        put32(templates, 16, 0x4f425244); // OBRD
        put32(templates, 24, 0x4f42444e); // OBDN
        put32(templates, 32, 0x78);
        put32(templates, 36, 1);
        real(templates, 40, 2);
        real(templates, 44, 4);
        put32(templates, 52, 5);
        put32(templates, 76, 5);
    }
    LegendsN64Obelisks decode() const { return decodeLegendsN64Obelisks(placements, templates); }
};
} // namespace

TEST_CASE("N64 obelisks use the placement identity and native scaled cylinder",
          "[legends][n64][obelisk][formats]") {
    Fixture fixture;
    const auto decoded = fixture.decode();
    REQUIRE(decoded.obelisks.size() == 1);
    CHECK(decoded.warnings.empty());
    const auto& item = decoded.obelisks.front();
    CHECK(item.nativeIndex == 0);
    CHECK(item.templateKey == 0x2601);
    CHECK(item.identity == 6);
    CHECK(item.position == Vec3{10, 20, -30});
    CHECK(item.yaw == -.5f);
    CHECK(item.scale == 2);
    CHECK(item.radius == 4);
    CHECK(item.height == 8);
    CHECK(item.minimumPlayers == 2);
    CHECK(item.flags == 0x1009);
    CHECK(item.activationFlags == 0x02000000);
    CHECK(item.sound == 0x78);
    CHECK(item.idleSequence == "OBRD");
    CHECK(item.spentSequence == "OBDN");
    CHECK(item.groundProjected);
    CHECK(item.supported);
    for (const f32 value : {0.0f, -2.0f, .001f}) {
        real(fixture.placements, 108, value);
        const auto fallback = fixture.decode().obelisks.front();
        CHECK(fallback.scale == 1);
        CHECK(fallback.radius == 2);
        CHECK(fallback.height == 4);
    }
}

TEST_CASE("N64 obelisks retain unsupported trigger and identity metadata",
          "[legends][n64][obelisk][formats]") {
    Fixture fixture;
    SECTION("unknown identity") {
        put32(fixture.placements, 104, 8);
    }
    SECTION("native uninitialized marker") {
        put32(fixture.placements, 104, 0xadadadad);
    }
    SECTION("hidden trap") {
        fixture.placements[102] = 8;
    }
    SECTION("non-cylinder contact") {
        put32(fixture.templates, 36, 3);
    }
    SECTION("extra hit volume") {
        put32(fixture.templates, 52, 1);
    }
    SECTION("attack activated") {
        put32(fixture.templates, 8, 0x04000000);
    }
    SECTION("unimplemented effect sequence") {
        put32(fixture.templates, 20, 0x54455354);
    }
    SECTION("no spent model") {
        put32(fixture.templates, 24, 0);
    }
    SECTION("no floor projection") {
        put16(fixture.templates, 6, 0x1029);
    }
    const auto result = fixture.decode();
    REQUIRE(result.obelisks.size() == 1);
    CHECK_FALSE(result.obelisks.front().supported);
    REQUIRE(result.warnings.size() == 1);
}

TEST_CASE("N64 obelisk parser bounds malformed records before reading them",
          "[legends][n64][obelisk][formats]") {
    Fixture fixture;
    SECTION("truncated header") {
        fixture.placements.resize(91);
    }
    SECTION("truncated placements") {
        fixture.placements.resize(115);
    }
    SECTION("placement points into header") {
        put32(fixture.placements, 12, 0);
    }
    SECTION("placement offset overflow") {
        put32(fixture.placements, 12, 0xfffffff0);
    }
    SECTION("template budget") {
        put32(fixture.templates, 0, 4097);
    }
    SECTION("truncated template") {
        fixture.templates.resize(99);
    }
    SECTION("unknown template") {
        put16(fixture.placements, 100, 0xffff);
    }
    SECTION("duplicate template") {
        const auto duplicate = fixture.templates;
        fixture.templates.insert(fixture.templates.end(), duplicate.begin() + 4, duplicate.end());
        put32(fixture.templates, 0, 2);
    }
    SECTION("non-finite scale") {
        real(fixture.placements, 108, std::numeric_limits<f32>::infinity());
    }
    SECTION("overflowing dimensions") {
        real(fixture.templates, 40, std::numeric_limits<f32>::max());
    }
    SECTION("negative radius") {
        real(fixture.templates, 40, -1);
    }
    SECTION("bad sequence") {
        put32(fixture.templates, 16, 0x41424301);
    }
    CHECK_THROWS_AS(fixture.decode(), FormatError);
}

TEST_CASE("Native N64 corpus has seven uniquely identified authored obelisks",
          "[legends][n64][obelisk][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "items/generic/ptrap.rom")) {
        SKIP("Extracted N64 Legends assets unavailable");
    }
    const auto traps = readFile(root / "items/generic/ptrap.rom");
    const std::array<std::string, 7> levels{"mount1", "mount2",  "mount3", "town1",
                                            "town2",  "castle1", "castle6"};
    const std::array<u32, 7> indices{3, 1, 32, 7, 11, 71, 20};
    std::set<u32> identities;
    usize maps = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        CAPTURE(entry.path());
        const auto decoded = decodeLegendsN64Obelisks(readFile(entry.path() / "ipr.rom"), traps);
        CHECK(decoded.warnings.empty());
        ++maps;
        for (const auto& obelisk : decoded.obelisks) {
            REQUIRE(obelisk.identity >= 1);
            REQUIRE(obelisk.identity <= 7);
            CHECK(identities.insert(obelisk.identity).second);
            CHECK(entry.path().filename().string() == levels[obelisk.identity - 1]);
            CHECK(obelisk.nativeIndex == indices[obelisk.identity - 1]);
            CHECK(obelisk.supported);
            CHECK(obelisk.radius == 2);
            CHECK(obelisk.height == 4);
            CHECK(obelisk.idleSequence == "OBRD");
            CHECK(obelisk.spentSequence == "OBDN");
            CHECK(obelisk.sound == 0x78);
        }
    }
    CHECK(maps == 34);
    CHECK(identities.size() == 7);
}
