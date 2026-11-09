#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/level/LegendsDreamcastTower.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast advisor text retains the native script paragraphs",
          "[legends][dreamcast-tower][assets]") {
    StringTable text;
    REQUIRE(text.load(test::dataDirectory() / "text", "en"));
    const auto program =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/disc/1ST_READ.BIN";
    if (!std::filesystem::is_regular_file(program)) {
        SKIP("Native Dreamcast executable is not installed");
    }
    const auto bytes = readFile(program);
    constexpr u32 kBase = 0x8c010000;
    const auto word = [&](u32 address) {
        REQUIRE(address >= kBase);
        REQUIRE(static_cast<usize>(address - kBase) + 4 <= bytes.size());
        return readU32LE(bytes, address - kBase);
    };
    const auto nativeString = [&](u32 address) {
        REQUIRE(address >= kBase);
        usize at = address - kBase;
        std::string result;
        for (;;) {
            REQUIRE(at < bytes.size());
            const u8 character = bytes[at++];
            if (character == 0) {
                return result;
            }
            result += static_cast<char>(character);
        }
    };
    const std::array labels{"history", "obelisk", "rune", "enemy"};
    for (usize i = 0; i < labels.size(); ++i) {
        CHECK(text.get(std::format("legends.dreamcast.sumner.topic.{}", labels[i])) ==
              nativeString(word(0x8c147fd4 + static_cast<u32>(i) * 4)));
    }
    CHECK(text.get("legends.dreamcast.sumner.topic.exit") == nativeString(0x8c11d534));
    struct Table {
        u32 address;
        std::string_view name;
        u32 count;
    };
    for (const auto table :
         std::array{Table{0x8c147e8c, "obelisk", 10}, Table{0x8c147edc, "rune", 14},
                    Table{0x8c147f4c, "enemy", 5}, Table{0x8c147f74, "history", 12}}) {
        for (u32 entry = 0; entry < table.count; ++entry) {
            const auto key = std::format("legends.dreamcast.sumner.{}.{}", table.name, entry);
            CAPTURE(key);
            REQUIRE(text.has(key));
            const u32 lines = word(table.address + entry * 8 + 4);
            REQUIRE(lines < 20);
            const u32 pointers = word(table.address + entry * 8);
            std::string paragraph;
            for (u32 line = 0; line < lines; ++line) {
                if (line != 0) {
                    paragraph += ' ';
                }
                paragraph += nativeString(word(pointers + line * 4));
            }
            CHECK(text.get(key) == paragraph);
        }
    }
}

TEST_CASE("Dreamcast tower restores only its authored gates and collected decorations",
          "[legends][dreamcast-tower]") {
    formats::LegendsMap map;
    for (const auto name : {"T1XSNS_DRFIE#0", "T1TRAPW02", "T1XSNS_DRFIE#2", "T1TRAPW04",
                            "T1XSNS_DRFIE#3", "T1TRAPW05", "T1XSNS_DRFIE#1", "T1TRAPW03",
                            "T1RUNEA1", "T1RUNEB1", "T1SHARD01", "T1SHARD02", "WALL", "CHILD"}) {
        formats::LegendsMapObject object;
        object.name = name;
        map.objects.push_back(object);
    }
    map.objects.back().parent = 0;
    LegendsDreamcastTower tower{map};
    WorldCollision collision;
    for (usize i = 0; i < 8; ++i) {
        CHECK(tower.visible(i));
    }
    for (usize i = 8; i < 12; ++i) {
        CHECK_FALSE(tower.visible(i));
    }
    CHECK(tower.visible(12));
    CHECK(tower.visible(999));
    LegendsProgress progress;
    progress.obelisks = 0x18; // Two out of three is not enough.
    tower.synchronize(progress);
    CHECK_FALSE(tower.gateOpen(0));
    progress.obelisks |= 0x20;
    progress.runestones = 1;
    progress.shards = 4;
    tower.synchronize(progress);
    tower.syncCollision(collision);
    CHECK(tower.gateOpen(0));
    CHECK_FALSE(tower.gateOpen(1));
    CHECK_FALSE(tower.visible(0));
    CHECK_FALSE(tower.visible(1));
    CHECK_FALSE(tower.visible(13));
    CHECK_FALSE(collision.solid(0));
    CHECK_FALSE(collision.solid(13));
    CHECK(collision.solid(2));
    CHECK(tower.visible(8));
    CHECK_FALSE(tower.visible(9));
    CHECK_FALSE(tower.visible(10));
    CHECK(tower.visible(11));
    progress.obelisks = 0x1ff;
    progress.runestones = 0x1fff;
    progress.shards = 0x3e;
    tower.synchronize(progress);
    for (usize gate = 0; gate < 4; ++gate) {
        CHECK(tower.gateOpen(gate));
    }
    for (usize object = 8; object < 12; ++object) {
        CHECK(tower.visible(object));
    }
    CHECK_FALSE(tower.gateOpen(4));
    tower.synchronize({});
    tower.syncCollision(collision);
    CHECK(tower.visible(0));
    CHECK(collision.solid(0));
    CHECK(collision.solid(13));
}

TEST_CASE("Dreamcast realm gates apply the executable's obelisk masks without a ceremony",
          "[legends][dreamcast-tower][dreamcast-native-gates][assets]") {
    const auto program =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/disc/1ST_READ.BIN";
    if (!std::filesystem::is_regular_file(program)) {
        SKIP("Native Dreamcast executable is not installed");
    }
    const auto bytes = readFile(program);
    constexpr u32 kBase = 0x8c010000;
    const auto word = [&](u32 address) {
        REQUIRE(address >= kBase);
        REQUIRE(static_cast<usize>(address - kBase) + 4 <= bytes.size());
        return readU32LE(bytes, address - kBase);
    };
    const auto nativeString = [&](u32 address) {
        REQUIRE(address >= kBase);
        usize at = address - kBase;
        std::string result;
        for (;;) {
            REQUIRE(at < bytes.size());
            const u8 character = bytes[at++];
            if (character == 0) {
                return result;
            }
            result += static_cast<char>(character);
        }
    };

    // TowerInit's gate-name literal pool and mask table are independent of the
    // adapter constants. 8c0659e8..8c065a38 changes visibility/collision directly;
    // InitLevel calls it before the ordinary Sumner create/show/talk controller.
    formats::LegendsMap map;
    for (u32 pair = 0; pair < 8; ++pair) {
        formats::LegendsMapObject object;
        object.name = nativeString(word(0x8c065acc + pair * 4));
        map.objects.push_back(std::move(object));
    }
    for (s32 parent = 0; parent < 8; ++parent) {
        formats::LegendsMapObject child;
        child.parent = parent;
        map.objects.push_back(std::move(child));
    }
    const std::array<u32, 3> masks{word(0x8c11bc08), word(0x8c11bc0c), word(0x8c11bc10)};
    REQUIRE(masks == std::array<u32, 3>{0x38, 0x7, 0x1c0});
    LegendsDreamcastTower tower{map};
    WorldCollision collision;
    for (u64 obelisks = 0; obelisks < 0x200; ++obelisks) {
        for (const u64 skorne : {u64{0}, u64{0x20}}) {
            CAPTURE(obelisks, skorne);
            LegendsProgress progress;
            progress.obelisks = obelisks;
            progress.shards = skorne;
            tower.synchronize(progress);
            tower.syncCollision(collision);
            for (usize gate = 0; gate < 4; ++gate) {
                const bool open =
                    gate < masks.size() ? (obelisks & masks[gate]) == masks[gate] : skorne != 0;
                CHECK(tower.gateOpen(gate) == open);
                for (usize half = 0; half < 2; ++half) {
                    const usize object = gate * 2 + half;
                    CHECK(tower.visible(object) == !open);
                    CHECK(collision.solid(static_cast<s32>(object)) == !open);
                    CHECK(tower.visible(object + 8) == !open);
                    CHECK(collision.solid(static_cast<s32>(object + 8)) == !open);
                }
            }
        }
    }
}

TEST_CASE("Dreamcast stained glass keeps its broken remainder and repairs each native shard",
          "[legends][dreamcast-tower][dreamcast-windows][assets]") {
    const auto extraction = test::dataDirectory().parent_path() / "assets/dreamcast/extracted";
    const auto directory = extraction / "native/levels/levelt1";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Dreamcast tower extraction is not installed");
    }
    const auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"),
                                                         readFile(directory / "objects.rom"));
    const auto index = [&](std::string_view name) {
        const auto found = std::ranges::find(map.objects, name, &formats::LegendsMapObject::name);
        REQUIRE(found != map.objects.end());
        return static_cast<usize>(found - map.objects.begin());
    };
    struct Piece {
        std::string_view parent;
        std::string_view mesh;
        u64 mask;
    };
    const std::array pieces{
        Piece{"T1SHARD01", "T1NS_POLYSUR#0", 2}, Piece{"T1SHARD02", "T1NS_POLYSUR#1", 4},
        Piece{"T1SHARD02", "T1NS_POLYSUR#2", 4}, Piece{"T1SHARD03", "T1NS_POLYSUR#3", 8},
        Piece{"T1SHARD04", "T1NS_POLYSUR#4", 16}};
    for (const auto& piece : pieces) {
        CAPTURE(piece.parent, piece.mesh);
        const auto& mesh = map.objects[index(piece.mesh)];
        REQUIRE(mesh.parent == static_cast<s32>(index(piece.parent)));
        REQUIRE_FALSE(mesh.mesh.parts.empty());
    }
    const usize broken = index("T1NS_STAINEDGLA"), frame = index("T1GROUP89");
    REQUIRE(map.objects[broken].parent == static_cast<s32>(frame));
    LegendsDreamcastTower tower{map};
    for (u64 owned = 0; owned < 128; ++owned) {
        CAPTURE(owned);
        LegendsProgress progress;
        progress.shards = owned;
        tower.synchronize(progress);
        CHECK(tower.visible(broken));
        CHECK(tower.visible(frame));
        for (const auto& piece : pieces) {
            CAPTURE(piece.mesh);
            const bool repaired = (owned & piece.mask) != 0;
            CHECK(tower.visible(index(piece.parent)) == repaired);
            CHECK(tower.visible(index(piece.mesh)) == repaired);
        }
    }
    tower.synchronize({});
    CHECK(tower.visible(broken));
    for (const auto& piece : pieces) {
        CHECK_FALSE(tower.visible(index(piece.mesh)));
    }
}

TEST_CASE("Dreamcast Sumner chooses the native hint order and history chapters",
          "[legends][dreamcast-tower]") {
    LegendsProgress progress;
    CHECK(LegendsDreamcastTower::advice(0, progress).size() == 5);
    CHECK(LegendsDreamcastTower::advice(1, progress) ==
          std::vector<std::string>{"legends.dreamcast.sumner.obelisk.0"});
    progress.obelisks = 8;
    CHECK(LegendsDreamcastTower::advice(1, progress).front().ends_with(".1"));
    progress.obelisks = 0x38;
    CHECK(LegendsDreamcastTower::advice(0, progress) ==
          std::vector<std::string>{"legends.dreamcast.sumner.history.5"});
    progress.runestones = 7; // Castle runes do not satisfy the first Mountain hint.
    CHECK(LegendsDreamcastTower::advice(2, progress).front().ends_with(".0"));
    progress.runestones |= 8;
    CHECK(LegendsDreamcastTower::advice(2, progress).front().ends_with(".1"));
    progress.shards = 2; // Chimera does not satisfy the Dragon hint.
    CHECK(LegendsDreamcastTower::advice(3, progress).front().ends_with(".0"));
    progress.shards |= 4;
    CHECK(LegendsDreamcastTower::advice(3, progress).front().ends_with(".2"));
    progress.obelisks = 0x1ff;
    progress.runestones = 0x1fff;
    progress.shards = 0x1e;
    CHECK(LegendsDreamcastTower::advice(1, progress).front().ends_with(".9"));
    CHECK(LegendsDreamcastTower::advice(2, progress).front().ends_with(".13"));
    CHECK(LegendsDreamcastTower::advice(3, progress).front().ends_with(".4"));
    CHECK(LegendsDreamcastTower::advice(0, progress).front().ends_with(".8"));
    progress.shards |= 0x20;
    CHECK(LegendsDreamcastTower::advice(0, progress) ==
          std::vector<std::string>{"legends.dreamcast.sumner.history.9",
                                   "legends.dreamcast.sumner.history.10"});
    progress.shards |= 0x40;
    CHECK(LegendsDreamcastTower::advice(0, progress).front().ends_with(".11"));
    CHECK(LegendsDreamcastTower::advice(-1, progress).empty());
    CHECK(LegendsDreamcastTower::advice(4, progress).empty());

    // The executable nests later chapters behind the earlier tower gates.
    // Out-of-order developer unlocks or imported saves cannot skip those gates.
    progress.obelisks = 0x1c7;
    progress.shards = 0x7e;
    CHECK(LegendsDreamcastTower::advice(0, progress).size() == 5);
    progress.obelisks = 0x1f8;
    CHECK(LegendsDreamcastTower::advice(0, progress) ==
          std::vector<std::string>{"legends.dreamcast.sumner.history.5"});
    progress.obelisks = 0x3f;
    CHECK(LegendsDreamcastTower::advice(0, progress).front().ends_with(".6"));
    progress.obelisks = 0x1ff;
    progress.shards = 0x60;
    CHECK(LegendsDreamcastTower::advice(0, progress).front().ends_with(".7"));
    progress.shards = 0x5e;
    CHECK(LegendsDreamcastTower::advice(0, progress).front().ends_with(".8"));
    // Hints update immediately when ownership changes, independently of history.
    progress.obelisks = 0;
    progress.runestones = 0;
    CHECK(LegendsDreamcastTower::advice(1, progress).front().ends_with(".0"));
    CHECK(LegendsDreamcastTower::advice(2, progress).front().ends_with(".0"));
}

TEST_CASE("Dreamcast Sumner requires a fresh nearby interaction without modal loops",
          "[legends][dreamcast-tower]") {
    LegendsDreamcastSumner sumner;
    CHECK(sumner.yaw() == kPi);
    std::array visitors{DreamcastSumnerVisitor{2, {0, 0, 9.75f}}};
    CHECK_FALSE(sumner.update(visitors, 1.0f / 30, false));
    visitors[0].interactPressed = true;
    CHECK(sumner.update(visitors, 1.0f / 30, false) == 2);
    visitors[0].interactPressed = false;
    CHECK_FALSE(sumner.update(visitors, 1.0f / 30, false));
    visitors[0].position = {3, 0, 9.75f};
    const auto old = sumner.yaw();
    CHECK_FALSE(sumner.update(visitors, 1.0f / 30, false));
    CHECK(std::abs(std::remainder(sumner.yaw() - old, 2 * kPi)) == Catch::Approx(10 * kPi / 180));
    for (int tick = 0; tick < 10; ++tick) {
        sumner.update(visitors, 1.0f / 30, true);
    }
    CHECK(sumner.yaw() == Catch::Approx(std::atan2(3.0f, -6.25f)));
    visitors[0].position.x = 5;
    visitors[0].interactPressed = true;
    CHECK_FALSE(sumner.update(visitors, .1f, false));
    visitors[0].position.x = 0;
    CHECK_FALSE(sumner.update(visitors, .1f, true));
    visitors[0].interactPressed = false;
    CHECK_FALSE(sumner.update(visitors, .1f, false)); // Blocked presses are not queued.
    visitors[0].interactPressed = true;
    CHECK(sumner.update(visitors, .1f, false) == 2);
    CHECK_FALSE(sumner.update({}, .1f, false));
    std::array party{DreamcastSumnerVisitor{3, {0, 0, 9.75f}, true},
                     DreamcastSumnerVisitor{1, {0, 0, 9.75f}, true}};
    CHECK(sumner.update(party, .1f, false) == 1); // Stable tie, independent of roster order.
    party[1].interactPressed = false;
    CHECK(sumner.update(party, .1f, false) == 3); // An idle visitor cannot steal the request.
    CHECK_FALSE(sumner.update(party, std::numeric_limits<f32>::quiet_NaN(), false));
    CHECK_FALSE(sumner.update({}, 1, false));
    CHECK(std::abs(sumner.yaw()) == Catch::Approx(kPi));
}

} // namespace
