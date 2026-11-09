#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/level/LegendsTowerCeremony.h"

namespace {
using namespace gdl;
using namespace gdl::game;

LegendsTowerCeremony ceremony() {
    const auto path =
        test::dataDirectory().parent_path() / "assets/n64/extracted/_program/Tower.bin";
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("Native N64 Tower overlay is not installed");
    }
    auto result = LegendsTowerCeremony::decode(readFile(path));
    result.bindSequences({{"REDY", 50, 1.0f / 30},
                          {"MKRU", 40, 1.0f / 30},
                          {"SHD1", 45, 1.0f / 30},
                          {"SHDR", 30, 1.0f / 30},
                          {"STRT", 45, 1.0f / 30},
                          {"ODOR", 30, 1.0f / 30}});
    return result;
}

void ticks(LegendsTowerCeremony& value, s32 count) {
    for (s32 i = 0; i < count; ++i) {
        value.update(1.0f / 30);
    }
}
} // namespace

TEST_CASE("N64 active podium state dispatches the six-choice Sumner menu with its heading",
          "[legends][sumner][tower-ceremony][assets]") {
    const auto path =
        test::dataDirectory().parent_path() / "assets/n64/extracted/_program/game.bin";
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("Native N64 program is not installed");
    }
    const auto bytes = readFile(path);
    const auto word = [&](u32 address) { return readU32BE(bytes, address - 0xe0000400U); };
    // Independent call-site evidence, not another copy of the menu parser's
    // expected labels: state30 -> handler -> menu6000 -> actual menu renderer.
    CHECK(word(0xe00aa380 + (0x30 - 1) * 4) == 0xe004553c);
    CHECK(word(0xe0045548) == 0x24020030); // li v0,30
    CHECK(word(0xe0045564) == 0x3c04e004); // lui a0,e004
    CHECK(word(0xe004556c) == 0x24846000); // addiu a0,a0,6000
    CHECK(((word(0xe0045584) & 0x03ffffffU) << 2) == (0xe0041894U & 0x0fffffffU));
    CHECK(word(0xe0046000) == 441); // "Sumner", drawn with font8 by e0041894.
    const auto options = word(0xe0046050);
    constexpr std::array<u32, 6> ids{426, 430, 442, 443, 444, 445};
    constexpr std::array<u32, 6> actions{0x11, 0x15, 0x2e, 0xd, 0x2f, 2};
    for (usize i = 0; i < ids.size(); ++i) {
        CHECK(word(options + static_cast<u32>(i) * 32) == ids[i]);
        CHECK(word(options + static_cast<u32>(i) * 32 + 4) == actions[i]);
    }
    CHECK(word(options + 6 * 32) == 0xffffffffU);
}

TEST_CASE("N64 ceremonies distinguish installed progress and follow native dispatch priority",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    LegendsProgress old;
    old.runestones = 1;
    old.shards = 1;
    LegendsProgress now = old;
    now.runestones = 0x1fff;
    now.shards = 15;
    now.obelisks = 255;
    value.begin(old, now);
    CHECK_FALSE(value.active());
    REQUIRE(value.entries().size() == 19);
    for (usize i = 0; i < 12; ++i) {
        CHECK(value.entries()[i].kind == LegendsTowerCeremony::Kind::Rune);
        CHECK(value.entries()[i].index == i + 1);
        CHECK(value.entries()[i].captions == std::vector<u32>{542U + static_cast<u32>(i)});
    }
    constexpr std::array<usize, 4> order{1, 3, 2, 0};
    for (usize i = 0; i < 4; ++i) {
        CHECK(value.entries()[12 + i].kind == LegendsTowerCeremony::Kind::Door);
        CHECK(value.entries()[12 + i].index == order[i]);
    }
    CHECK(value.entries()[16].index == 3);
    CHECK(value.entries()[17].index == 2);
    CHECK(value.entries()[18].index == 1);
    CHECK(value.displayedProgress().runestones == old.runestones);
    CHECK(value.displayedProgress().shards == old.shards);
    CHECK(now.runestones == 0x1fff); // Presentation does not own/save the award.
    value.begin(now, now);
    CHECK_FALSE(value.pending());
    CHECK(value.displayedProgress() == now);
}

TEST_CASE("N64 runestone ceremony uses its recorded flight and restores control after its camera",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    LegendsProgress now;
    now.runestones = 1;
    value.begin({}, now);
    WorldCamera original;
    original.position = {20, 30, 40};
    original.pitch = .4f;
    original.yaw = .8f;
    value.start(original, {0, 0, 0});
    CHECK(value.caption() == 541);
    ticks(value, 49);
    CHECK(value.gesture() == "REDY");
    CHECK_FALSE(value.runeInFlight());
    ticks(value, 1);
    CHECK(value.gesture() == "MKRU");
    ticks(value, 27);
    CHECK_FALSE(value.runeInFlight());
    ticks(value, 1);
    REQUIRE(value.runeInFlight());
    const Vec3 above{value.runePlacement()[3]};
    CHECK(value.displayedProgress().runestones == 0);
    ticks(value, 61);
    const Vec3 floor{value.runePlacement()[3]};
    CHECK(above.y > floor.y + 4);
    ticks(value, 1);
    CHECK(value.displayedProgress().runestones == 1);
    CHECK_FALSE(value.runeInFlight());
    CHECK(value.active());
    ticks(value, 100);
    CHECK_FALSE(value.active());
    CHECK_FALSE(value.pending());
    CHECK(glm::distance(value.camera().position, original.position) < .001f);
    const auto sounds = value.takeSounds();
    CHECK(sounds == std::vector<u32>{0x68, 0x8a});
    CHECK(value.takeSounds().empty());
}

TEST_CASE("N64 shard gesture cannot install the piece before its animation completes",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    LegendsProgress now;
    now.shards = 1;
    value.begin({}, now);
    value.start({}, {});
    ticks(value, 91);
    CHECK(value.displayedProgress().shards == 0);
    ticks(value, 1);
    CHECK(value.gesture() == "SHD1");
    ticks(value, 43);
    CHECK(value.displayedProgress().shards == 0);
    ticks(value, 1);
    CHECK(value.displayedProgress().shards == 1);
    CHECK(value.shardOpacity() == 0);
    ticks(value, 21 + 25);
    CHECK(value.gesture() == "SHDR");
    CHECK(value.shardOpacity() == Catch::Approx(47.0f / 255));
    ticks(value, 120);
    CHECK_FALSE(value.active());
}

TEST_CASE("N64 gate animation holds the staff spell before releasing the actual door",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    LegendsProgress now;
    now.obelisks = 7;
    value.begin({}, now);
    value.start({}, {});
    ticks(value, 47);
    CHECK(value.gesture() == "STRT");
    CHECK(value.gestureFrame() == 44);
    ticks(value, 44);
    CHECK(value.gestureFrame() == 0);
    ticks(value, 44);
    CHECK(value.gesture() == "ODOR");
    ticks(value, 15);
    CHECK(value.gestureFrame() == 15);
    ticks(value, 49);
    CHECK(value.gestureFrame() == 15);
    CHECK(value.displayedProgress().obelisks == 0);
    ticks(value, 1 + 5);
    CHECK(value.displayedProgress().obelisks == 7);
    ticks(value, 120);
    CHECK_FALSE(value.active());
}

TEST_CASE("N64 ceremony clocks ignore invalid steps and are independent of render rate",
          "[legends][tower-ceremony][assets]") {
    auto a = ceremony();
    auto b = a;
    LegendsProgress now;
    now.shards = 1;
    a.begin({}, now);
    b.begin({}, now);
    a.start({}, {});
    b.start({}, {});
    a.update(-1);
    a.update(std::numeric_limits<f32>::quiet_NaN());
    CHECK(a.gestureFrame() == 0);
    for (s32 i = 0; i < 100; ++i) {
        a.update(1.0f / 30);
        b.update(1.0f / 60);
        b.update(1.0f / 60);
    }
    CHECK(a.gesture() == b.gesture());
    CHECK(a.gestureFrame() == b.gestureFrame());
    CHECK(a.displayedProgress() == b.displayedProgress());
    CHECK(a.camera() == b.camera());
}
