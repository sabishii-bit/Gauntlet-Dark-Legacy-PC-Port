#include <algorithm>
#include <array>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/level/LegendsUnlocks.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr usize kRealms = 0xe006c360U - 0xe0000400U;
constexpr usize kLevels = 0xe006c468U - 0xe0000400U;

void put32(std::vector<u8>& bytes, usize at, u32 value) {
    for (usize i = 0; i < 4; ++i) {
        bytes.at(at + i) = static_cast<u8>(value >> ((3 - i) * 8));
    }
}

std::vector<u8> program() {
    std::vector<u8> bytes(kLevels + 1024);
    usize text = kLevels + 800;
    auto name = [&](usize pointer, std::string_view value) {
        put32(bytes, pointer, static_cast<u32>(text) + 0xe0000400U);
        std::ranges::copy(value, bytes.begin() + static_cast<std::ptrdiff_t>(text));
        text += value.size() + 1;
    };
    put32(bytes, kRealms, 3);
    name(kRealms + 8, "mount");
    put32(bytes, kRealms + 24, 0);
    name(kRealms + 32, "castle");
    put32(bytes, kRealms + 40, 7);
    put32(bytes, kRealms + 48, 16);
    constexpr std::array<u32, 4> kRealms{3, 3, 0, 0};
    constexpr std::array<std::string_view, 4> kSuffixes{"1", "2", "1", "6"};
    for (usize i = 0; i < kRealms.size(); ++i) {
        const usize at = kLevels + i * 40;
        put32(bytes, at, kRealms[i]);
        put32(bytes, at + 4, static_cast<u32>(i % 2));
        name(at + 12, kSuffixes[i]);
    }
    put32(bytes, kLevels + 160, 16);
    return bytes;
}

TEST_CASE("N64 stage admission requires realm keys and preceding native stages",
          "[legends][unlocks]") {
    const auto policy = LegendsN64Unlocks::decode(program());
    LegendsProgress progress;
    REQUIRE(policy.stages().size() == 4);
    CHECK(policy.allows("MOUNT1", progress));
    CHECK_FALSE(policy.allows("mount2", progress));
    CHECK_FALSE(policy.allows("castle1", progress));
    CHECK_FALSE(policy.allows("castle6", progress));
    CHECK_FALSE(policy.allows("castle2", progress));
    CHECK_FALSE(policy.allows("../mount1", progress));
    CHECK_FALSE(LegendsN64Unlocks{}.allows("mount1", progress));
    REQUIRE(progress.recordCompleted("mount1"));
    CHECK(policy.allows("mount2", progress));
    progress.obelisks = 3;
    CHECK_FALSE(policy.allows("castle1", progress));
    progress.obelisks = 7;
    CHECK(policy.allows("castle1", progress));
    CHECK_FALSE(policy.allows("castle6", progress));
    REQUIRE(progress.recordCompleted("castle1"));
    CHECK(policy.allows("castle6", progress));
    progress.obelisks = 0;
    CHECK_FALSE(policy.allows("castle6", progress));
    progress.completedMaps.emplace_back("BAD");
    CHECK_FALSE(policy.allows("mount1", progress));
}

TEST_CASE("Native tower developer unlock is bounded to its own policy roster",
          "[legends][unlocks][native-progress-flow]") {
    const auto policy = LegendsN64Unlocks::decode(program());
    LegendsProgress progress;
    REQUIRE(policy.unlockAll(progress));
    CHECK(progress.runestones == 0x1fff);
    CHECK(progress.shards == 15);
    CHECK(progress.obelisks == 255);
    CHECK(progress.legendItems == 0);
    for (const auto& stage : policy.stages()) {
        CHECK(policy.allows(stage.map, progress));
    }
    const auto complete = progress;
    REQUIRE(policy.unlockAll(progress));
    CHECK(progress == complete);
    CHECK_FALSE(LegendsN64Unlocks{}.unlockAll(progress));
    CHECK(progress == complete);
    LegendsProgress full;
    for (usize i = 0; i < LegendsProgress::kMaxCompletedMaps; ++i) {
        REQUIRE(full.recordCompleted("custom" + std::to_string(i)));
    }
    const auto before = full;
    CHECK_FALSE(policy.unlockAll(full));
    CHECK(full == before);
}

TEST_CASE("N64 stage policy refuses malformed or ambiguous program tables", "[legends][unlocks]") {
    auto bytes = program();
    SECTION("truncated program") {
        bytes.resize(kLevels + 159);
    }
    SECTION("invalid realm") {
        put32(bytes, kRealms, 17);
    }
    SECTION("duplicate realm") {
        put32(bytes, kRealms + 24, 3);
    }
    SECTION("unknown realm") {
        put32(bytes, kLevels, 7);
    }
    SECTION("duplicate stage") {
        put32(bytes, kLevels + 44, 0);
    }
    SECTION("invalid stage") {
        put32(bytes, kLevels + 4, 11);
    }
    SECTION("invalid pointer") {
        put32(bytes, kRealms + 8, 0xffffffffU);
    }
    SECTION("unsafe name") {
        bytes[kLevels + 800] = '/';
    }
    SECTION("missing terminator") {
        for (usize i = 160; i < 800; i += 40) {
            put32(bytes, kLevels + i, 3);
        }
    }
    CHECK_THROWS_AS(LegendsN64Unlocks::decode(bytes), FormatError);
}

TEST_CASE("Native N64 table order governs Castle Ice and boss portal requirements",
          "[legends][unlocks][assets]") {
    const auto path =
        test::dataDirectory().parent_path() / "assets/n64/extracted/_program/game.bin";
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    const auto policy = LegendsN64Unlocks::decode(readFile(path));
    REQUIRE(policy.stages().size() == 37);
    CHECK(policy.isTower("TOWER2"));
    CHECK_FALSE(policy.isTower("_test1"));
    CHECK_FALSE(policy.isTower("mount1"));
    CHECK_FALSE(policy.isTower("tower1"));
    const auto find = [&](std::string_view map) -> const LegendsStageUnlock& {
        const auto found = std::ranges::find(policy.stages(), map, &LegendsStageUnlock::map);
        REQUIRE(found != policy.stages().end());
        return *found;
    };
    CHECK(find("castle3").preceding == std::vector<std::string>{"castle1", "castle6"});
    CHECK(find("ice6").preceding ==
          std::vector<std::string>{"ice1", "ice2", "ice3", "ice4", "ice5"});
    CHECK(find("battle1").requiredObelisks == 0x80);
    CHECK(find("hell1").requiredObelisks == 0x100);
    CHECK(find("boss1").requiredObelisks == 0x200);
    LegendsProgress progress;
    CHECK(policy.allows("mount1", progress));
    CHECK_FALSE(policy.allows("mount6", progress));
    progress.obelisks = ~u64{0};
    for (const auto& stage : policy.stages()) {
        CAPTURE(stage.map);
        CHECK(policy.allows(stage.map, progress) == stage.preceding.empty());
    }
    for (const auto& stage : policy.stages()) {
        REQUIRE(progress.recordCompleted(stage.map));
    }
    for (const auto& stage : policy.stages()) {
        CHECK(policy.allows(stage.map, progress));
    }
    const auto before = progress;
    CHECK_FALSE(policy.allows("mount7", progress));
    CHECK(progress == before);
}
} // namespace
