#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/level/LegendsTowerCeremony.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr f32 kTick = 1.0f / 30;

/** Sumner's WIZMOVES headers: REDY loops; rate 16 is a frame an update, ODOR's 32 half. */
std::vector<LegendsTowerCeremony::Sequence> nativeSequences() {
    return {{"REDY", 60, 1.0f / 30, true}, {"MKRU", 30, 1.0f / 30}, {"SHD1", 19, 1.0f / 30},
            {"SHDR", 11, 1.0f / 30},       {"STRT", 60, 1.0f / 30}, {"ODOR", 25, 1.0f / 15}};
}

/** Without its overlay a ceremony keeps the scripts' clocks; only the captions and the
 * shots' focus points come from Tower.bin. A replacement without frames leaves its gesture
 * out, as a WIZMOVES that lacks it would. */
LegendsTowerCeremony
nativeCeremony(std::initializer_list<LegendsTowerCeremony::Sequence> replacements = {}) {
    auto sequences = nativeSequences();
    for (const auto& replacement : replacements) {
        std::erase_if(sequences,
                      [&](const auto& sequence) { return sequence.name == replacement.name; });
        if (replacement.frames > 0) {
            sequences.push_back(replacement);
        }
    }
    LegendsTowerCeremony result;
    result.bindSequences(std::move(sequences));
    return result;
}

LegendsTowerCeremony ceremony() {
    const auto path =
        test::dataDirectory().parent_path() / "assets/n64/extracted/_program/Tower.bin";
    if (!std::filesystem::is_regular_file(path)) {
        SKIP("Native N64 Tower overlay is not installed");
    }
    auto result = LegendsTowerCeremony::decode(readFile(path));
    result.bindSequences({{"REDY", 50, 1.0f / 30, true},
                          {"MKRU", 40, 1.0f / 30},
                          {"SHD1", 45, 1.0f / 30},
                          {"SHDR", 30, 1.0f / 30},
                          {"STRT", 45, 1.0f / 30},
                          {"ODOR", 30, 1.0f / 30}});
    return result;
}

void ticks(LegendsTowerCeremony& value, s32 count) {
    for (s32 i = 0; i < count; ++i) {
        value.update(kTick);
    }
}

std::vector<u32> sounds(std::initializer_list<u32> ids) {
    return ids;
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
    constexpr std::array<u32, 6> kIds{426, 430, 442, 443, 444, 445};
    constexpr std::array<u32, 6> kActions{0x11, 0x15, 0x2e, 0xd, 0x2f, 2};
    for (usize i = 0; i < kIds.size(); ++i) {
        CHECK(word(options + static_cast<u32>(i) * 32) == kIds[i]);
        CHECK(word(options + static_cast<u32>(i) * 32 + 4) == kActions[i]);
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
        // Only the stone that makes all thirteen returns as it settles (e086af00).
        CHECK(value.entries()[i].completesRunes == (i == 11));
    }
    constexpr std::array<usize, 4> kOrder{1, 3, 2, 0};
    // The Ice door's second line is the empty 526, which the queue skips.
    const std::array<std::vector<u32>, 4> doorCaptions{
        std::vector<u32>{523, 524}, std::vector<u32>{529, 530}, std::vector<u32>{525},
        std::vector<u32>{527, 528}};
    for (usize i = 0; i < 4; ++i) {
        CHECK(value.entries()[12 + i].kind == LegendsTowerCeremony::Kind::Door);
        CHECK(value.entries()[12 + i].index == kOrder[i]);
        CHECK(value.entries()[12 + i].captions == doorCaptions[i]);
    }
    CHECK(value.entries()[16].index == 3);
    CHECK(value.entries()[16].captions == std::vector<u32>{535, 536});
    CHECK(value.entries()[17].index == 2);
    CHECK(value.entries()[17].captions == std::vector<u32>{537, 538});
    CHECK(value.entries()[18].index == 1);
    CHECK(value.entries()[18].captions == std::vector<u32>{533, 534});
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
    // Case 0 empties the caption queue; the Select update (tick 1) queues the stone's
    // caption and e086098c first shows it on tick 2.
    CHECK_FALSE(value.caption());
    ticks(value, 1);
    CHECK_FALSE(value.caption());
    ticks(value, 1);
    CHECK(value.caption() == 541);
    ticks(value, 49);
    CHECK(value.gesture() == "REDY");
    CHECK_FALSE(value.runeInFlight());
    ticks(value, 1);
    CHECK(value.gesture() == "MKRU");
    ticks(value, 29);
    CHECK_FALSE(value.runeInFlight());
    ticks(value, 1);
    REQUIRE(value.runeInFlight());
    const Vec3 above{value.runePlacement()[3]};
    CHECK(value.displayedProgress().runestones == 0);
    ticks(value, 9);
    CHECK(value.caption() == 541); // Its 90th caption update, tick 91.
    ticks(value, 1);
    CHECK_FALSE(value.caption());
    ticks(value, 51);
    const Vec3 floor{value.runePlacement()[3]};
    CHECK(above.y > floor.y + 4);
    ticks(value, 1);
    CHECK(value.displayedProgress().runestones == 1);
    CHECK_FALSE(value.runeInFlight());
    CHECK(value.active());
    ticks(value, 100);
    CHECK_FALSE(value.active());
    CHECK_FALSE(value.pending());
    CHECK(value.camera() == original);
    const auto heard = value.takeSounds();
    CHECK(heard == sounds({0x68, 0x8a}));
    CHECK(value.takeSounds().empty());
}

TEST_CASE("N64 runestone ceremony runs e086239c's clock after the dispatcher selects it",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress now;
    now.runestones = 1;
    value.begin({}, now);
    REQUIRE(value.entries().size() == 1);
    CHECK_FALSE(value.entries()[0].completesRunes);
    // start() is dispatcher case 0 with its pass, REDY held at frame 0; tick n below is the
    // nth update after case 0.
    value.start({}, {});
    CHECK(value.gesture() == "REDY");
    CHECK(value.gestureFrame() == 0);
    for (s32 tick = 1; tick <= 232; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        // Time 50 plays 0x68 and marks the spell, which starts on the next update.
        CHECK(value.takeSounds() == (tick == 51    ? sounds({0x68})
                                     : tick == 144 ? sounds({0x8a})
                                                   : sounds({})));
        if (tick == 52) {
            CHECK(value.gesture() == "MKRU");
            CHECK(value.gestureFrame() == 0);
        }
        if (tick == 53) {
            CHECK(value.gestureFrame() == .5f);
        }
        if (tick == 80) {
            CHECK(value.gestureFrame() == 14);
        }
        // Tick 81 sees frame 14 from 80's pass; the flight shows its first pose on 82.
        CHECK(value.runeInFlight() == (tick >= 82 && tick < 144));
        if (tick == 82) {
            CHECK(value.runeOpacity() == 0);
        }
        if (tick == 83) {
            CHECK(value.runeOpacity() == Catch::Approx(1.0f / 24));
        }
        // MKRU's end, read on 111, starts a REDY that inherits its once flag; e0860d1c
        // then restarts REDY looping, and it keeps cycling through the return.
        if (tick >= 111 && tick <= 170) {
            CHECK(value.gesture() == "REDY");
            CHECK(value.gestureFrame() == static_cast<f32>(tick - 111));
        }
        if (tick >= 171 && tick <= 231) {
            CHECK(value.gestureFrame() == static_cast<f32>((tick - 171) % 60));
        }
        CHECK(value.displayedProgress().runestones == (tick >= 144 ? 1U : 0U));
        CHECK(value.active() == (tick < 232));
    }
    CHECK(value.gesture() == "REDY");
    CHECK(value.gestureFrame() == 0);
}

TEST_CASE("N64 runestone completing thirteen returns on the update after it lands",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress before;
    before.runestones = 0x1ffe;
    LegendsProgress after;
    after.runestones = 0x1fff;
    value.begin(before, after);
    REQUIRE(value.entries().size() == 1);
    CHECK(value.entries()[0].completesRunes);
    value.start({}, {});
    for (s32 tick = 1; tick <= 202; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        CHECK(value.displayedProgress().runestones == (tick >= 144 ? 0x1fffU : 0x1ffeU));
        CHECK(value.active() == (tick < 202));
    }
}

TEST_CASE("N64 runestone ceremony releases the stone at frame 14 of whatever Sumner plays",
          "[legends][tower-ceremony]") {
    LegendsProgress now;
    now.runestones = 1;
    SECTION("MKRU is absent") {
        // e002f708 finds no MKRU on 52 and e0860d1c restarts REDY in that update; e086239c's
        // release test sees REDY's frame 14 on 67, so the flight shows from 68.
        auto value = nativeCeremony({{.name = "MKRU"}});
        value.begin({}, now);
        value.start({}, {});
        for (s32 tick = 1; tick <= 218; ++tick) {
            CAPTURE(tick);
            value.update(kTick);
            CHECK(value.takeSounds() == (tick == 51    ? sounds({0x68})
                                         : tick == 130 ? sounds({0x8a})
                                                       : sounds({})));
            CHECK(value.gesture() == "REDY");
            CHECK(value.runeInFlight() == (tick >= 68 && tick < 130));
            CHECK(value.displayedProgress().runestones == (tick >= 130 ? 1U : 0U));
            CHECK(value.active() == (tick < 218));
        }
    }
    SECTION("MKRU ends short of frame 14") {
        // A 10-frame MKRU ends at frame 9 on 70. Retail would wait for frame 14 forever; the
        // stone goes as the spell ends and flies from 72.
        auto value = nativeCeremony({{"MKRU", 10}});
        value.begin({}, now);
        value.start({}, {});
        for (s32 tick = 1; tick <= 222; ++tick) {
            CAPTURE(tick);
            value.update(kTick);
            CHECK(value.takeSounds() == (tick == 51    ? sounds({0x68})
                                         : tick == 134 ? sounds({0x8a})
                                                       : sounds({})));
            CHECK((value.gesture() == "MKRU") == (tick >= 52 && tick < 71));
            CHECK(value.runeInFlight() == (tick >= 72 && tick < 134));
            CHECK(value.displayedProgress().runestones == (tick >= 134 ? 1U : 0U));
            CHECK(value.active() == (tick < 222));
        }
    }
}

TEST_CASE("N64 door ceremony runs e0865c98's counts", "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress now;
    now.obelisks = 7;
    value.begin({}, now);
    REQUIRE(value.entries().size() == 1);
    value.start({}, {});
    for (s32 tick = 1; tick <= 358; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        // 0xb9 as the reversed STRT's own count passes 30; 0x6e once ODOR passes 16.
        CHECK(value.takeSounds() == (tick == 79    ? sounds({0xb9})
                                     : tick == 252 ? sounds({0x6e})
                                                   : sounds({})));
        if (tick == 48) {
            CHECK(value.gesture() == "STRT");
            CHECK(value.gestureFrame() == 59);
        }
        if (tick == 107) {
            CHECK(value.gestureFrame() == 0);
        }
        if (tick == 108) {
            CHECK(value.gesture() == "STRT");
            CHECK(value.gestureFrame() == 0);
        }
        if (tick == 167) {
            CHECK(value.gesture() == "STRT");
        }
        if (tick == 168) {
            CHECK(value.gesture() == "ODOR");
        }
        // Frame 15 holds for the 50-update countdown taken from 199, which sees it.
        if (tick >= 198 && tick <= 248) {
            CHECK(value.gestureFrame() == 15);
        }
        if (tick == 249) {
            CHECK(value.gestureFrame() == 15.5f);
        }
        // 258 sees 19.5; case 7 releases the door on 259.
        CHECK(value.displayedProgress().obelisks == (tick >= 259 ? 7U : 0U));
        CHECK((value.gesture() == "REDY") == (tick < 48 || tick >= 267));
        CHECK(value.active() == (tick < 358));
    }
}

TEST_CASE("N64 door ceremony lets REDY carry the door past a short or absent ODOR",
          "[legends][tower-ceremony]") {
    LegendsProgress now;
    now.obelisks = 7;
    SECTION("ODOR ends short of frame 19") {
        // An 18-frame ODOR holds at 15, sounds 0x6e and ends at frame 17 on 252. Case 6 reads
        // whatever Sumner plays, so the REDY that e0860d1c starts on 253 holds at 15 and
        // sounds 0x6e again; 324 sees 20 and case 7 releases the door on 325.
        auto value = nativeCeremony({{"ODOR", 18, 1.0f / 15}});
        value.begin({}, now);
        value.start({}, {});
        for (s32 tick = 1; tick <= 424; ++tick) {
            CAPTURE(tick);
            value.update(kTick);
            CHECK(value.takeSounds() == (tick == 79                   ? sounds({0xb9})
                                         : tick == 252 || tick == 321 ? sounds({0x6e})
                                                                      : sounds({})));
            CHECK((value.gesture() == "ODOR") == (tick >= 168 && tick < 253));
            if (tick >= 268 && tick <= 318) {
                CHECK(value.gesture() == "REDY");
                CHECK(value.gestureFrame() == 15);
            }
            CHECK(value.displayedProgress().obelisks == (tick >= 325 ? 7U : 0U));
            CHECK(value.active() == (tick < 424));
        }
    }
    SECTION("ODOR is absent") {
        // e002f708 finds no ODOR on 168 and e0860d1c restarts REDY in that update. REDY holds
        // at 15 from 183 and sounds 0x6e on 236; 239 sees 20 and case 7 releases the door on 240.
        auto value = nativeCeremony({{.name = "ODOR"}});
        value.begin({}, now);
        value.start({}, {});
        for (s32 tick = 1; tick <= 339; ++tick) {
            CAPTURE(tick);
            value.update(kTick);
            CHECK(value.takeSounds() == (tick == 79    ? sounds({0xb9})
                                         : tick == 236 ? sounds({0x6e})
                                                       : sounds({})));
            CHECK((value.gesture() == "REDY") == (tick < 48 || tick >= 168));
            if (tick >= 183 && tick <= 233) {
                CHECK(value.gestureFrame() == 15);
            }
            CHECK(value.displayedProgress().obelisks == (tick >= 240 ? 7U : 0U));
            CHECK(value.active() == (tick < 339));
        }
    }
}

TEST_CASE("N64 ceremonies hand back where retail would wait for a frame no gesture reaches",
          "[legends][tower-ceremony]") {
    SECTION("no gestures are bound") {
        // The rune's release test and the door's case 6 see only an ended gesture and move
        // on at once; the shard's cases wait for no frame.
        LegendsProgress rune;
        rune.runestones = 1;
        LegendsProgress door;
        door.obelisks = 7;
        LegendsProgress shard;
        shard.shards = 1;
        struct Visit {
            std::string_view kind;
            LegendsProgress after;
            s32 placed;   ///< the update the piece shows
            s32 handBack; ///< the update control returns
        };
        const std::array<Visit, 3> visits{
            {{"rune", rune, 116, 204}, {"door", door, 52, 151}, {"shard", shard, 95, 242}}};
        for (const auto& visit : visits) {
            LegendsTowerCeremony value;
            value.begin({}, visit.after);
            value.start({}, {});
            for (s32 tick = 1; tick <= visit.handBack; ++tick) {
                CAPTURE(visit.kind, tick);
                value.update(kTick);
                CHECK((value.displayedProgress() == visit.after) == (tick >= visit.placed));
                CHECK(value.active() == (tick < visit.handBack));
            }
        }
    }
    SECTION("REDY ends short of the door's frame 19 too") {
        // After an 18-frame ODOR, an 18-frame REDY loop holds at 15, then wraps from 17 on 321
        // without passing 19; 322 sees the wrap and the door goes on 323.
        auto value = nativeCeremony({{"ODOR", 18, 1.0f / 15}, {"REDY", 18, 1.0f / 30, true}});
        LegendsProgress now;
        now.obelisks = 7;
        value.begin({}, now);
        value.start({}, {});
        for (s32 tick = 1; tick <= 422; ++tick) {
            CAPTURE(tick);
            value.update(kTick);
            CHECK(value.takeSounds() == (tick == 79                   ? sounds({0xb9})
                                         : tick == 252 || tick == 321 ? sounds({0x6e})
                                                                      : sounds({})));
            CHECK(value.displayedProgress().obelisks == (tick >= 323 ? 7U : 0U));
            CHECK(value.active() == (tick < 422));
        }
    }
}

TEST_CASE("N64 shard ceremony runs e0863440's counts and keeps REDY cycling",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress now;
    now.shards = 1;
    value.begin({}, now);
    value.start({}, {});
    for (s32 tick = 1; tick <= 260; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        // The ready loop wraps instead of freezing at its last frame.
        if (tick >= 59 && tick <= 61) {
            CHECK(value.gesture() == "REDY");
            CHECK(value.gestureFrame() == static_cast<f32>(tick == 59 ? 59 : tick - 60));
        }
        CHECK((value.gesture() == "SHD1") == (tick >= 94 && tick < 161));
        CHECK(value.displayedProgress().shards == (tick >= 113 ? 1U : 0U));
        if (tick == 113) {
            CHECK(value.shardOpacity() == 0);
            CHECK(value.shardWhiteBlend() == 1);
        }
        CHECK(value.takeSounds() == (tick == 135 ? sounds({0x68}) : sounds({})));
        if (tick == 136) {
            CHECK(value.shardOpacity() == Catch::Approx(30.0f / 255));
        }
        if (tick == 139) {
            CHECK(value.shardOpacity() == 1);
        }
        if (tick == 160) {
            CHECK(value.shardOpacity() == Catch::Approx(47.0f / 255));
            CHECK(value.shardWhiteBlend() == 0);
        }
        CHECK((value.gesture() == "SHDR") == (tick >= 161 && tick < 172));
        CHECK(value.active() == (tick < 260));
    }
}

TEST_CASE("N64 last shard reveals the window on case 9's counts", "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress before;
    before.shards = 7;
    LegendsProgress after;
    after.shards = 15;
    value.begin(before, after);
    value.start({}, {});
    CHECK(value.cathedralBeamOpacity() == 0);
    for (s32 tick = 1; tick <= 394; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        // Case 7 (161) unhides the beam fully transparent; case 8 hands over on its 52nd
        // update, and case 9's counts 0 to 40 run from 214.
        const s32 count = std::clamp(tick - 214, 0, 40);
        const s32 transparency = (40 - count) * 255 / 40;
        CHECK(value.windowReveal() == Catch::Approx(static_cast<f32>(count) / 40));
        CHECK(value.cathedralBeamOpacity() ==
              Catch::Approx(static_cast<f32>(255 - transparency) / 255));
        CHECK(value.active() == (tick < 394));
    }
    CHECK(value.cathedralBeamOpacity() == 1);
    // e0862fc4 leaves a completed window's beam shown through later ceremonies.
    const LegendsProgress window = after;
    LegendsProgress rune = window;
    rune.runestones = 1;
    value.begin(window, rune);
    value.start({}, {});
    for (s32 tick = 1; tick <= 232; ++tick) {
        CAPTURE(tick);
        CHECK(value.cathedralBeamOpacity() == 1);
        value.update(kTick);
    }
    CHECK_FALSE(value.active());
}

TEST_CASE("N64 ceremonies select each entry on the update after the last one returns",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress now;
    now.runestones = 1;
    now.shards = 1;
    value.begin({}, now);
    REQUIRE(value.entries().size() == 2);
    value.start({}, {});
    // The rune returns on 175, so the shard's Select is 176 and its tick n is 175 + n.
    for (s32 tick = 1; tick <= 435; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        CHECK(value.displayedProgress().runestones == (tick >= 144 ? 1U : 0U));
        CHECK((value.gesture() == "SHD1") == (tick >= 175 + 94 && tick < 175 + 161));
        CHECK(value.displayedProgress().shards == (tick >= 175 + 113 ? 1U : 0U));
        CHECK(value.active() == (tick < 175 + 260));
    }
    // A rune, door and shard visit: the door returns on 476, the shard on 679.
    now.obelisks = 7;
    value.begin({}, now);
    REQUIRE(value.entries().size() == 3);
    value.start({}, {});
    for (s32 tick = 1; tick <= 736; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        CHECK(value.displayedProgress().obelisks == (tick >= 175 + 259 ? 7U : 0U));
        CHECK(value.active() == (tick < 736));
    }
}

TEST_CASE("N64 ceremony camera cuts to Sumner, moves on script updates and returns in 54",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress now;
    now.shards = 1;
    value.begin({}, now);
    WorldCamera original;
    original.position = {20, 30, 40};
    original.pitch = .4f;
    original.yaw = .8f;
    const Vec3 attention{1, 2, 3};
    value.start(original, attention);
    // e0860620 cuts every channel: nothing glides from the gameplay camera, whose yaw stays.
    const auto cut = value.camera();
    CHECK(value.camera(0) == cut);
    CHECK(value.camera(.5f) == cut);
    CHECK(cut.pitch == Catch::Approx(kPi / 6));
    CHECK(cut.yaw == original.yaw);
    CHECK(cut.roll == 0);
    CHECK(value.attention() == Vec3{0, 3, 0}); // Sumner, at the origin without the overlay
    CHECK(glm::distance(cut.position + cut.forward() * 30.0f, value.attention()) < 1e-4f);
    WorldCamera previous = cut;
    for (s32 tick = 1; tick <= 260; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        const auto camera = value.camera();
        // The shard shot, planned on 43, glides over .7, 1 and .3 s: 60 updates from 44.
        // The script returns on 203; Select (204) and case 0x28's planning update (205)
        // hold the camera, and the return ends on its 54th advance.
        CHECK((camera != previous) ==
              ((tick >= 44 && tick <= 103) || (tick >= 206 && tick <= 259)));
        if (tick == 103) {
            CHECK(camera.pitch == Catch::Approx(10 * kPi / 180));
        }
        if (tick == 258) {
            CHECK(camera != original);
        }
        if (tick == 259) {
            CHECK(camera == original);
            CHECK(value.attention() == attention);
        }
        CHECK(value.active() == (tick < 260));
        previous = camera;
    }
}

TEST_CASE("N64 shard gesture cannot install the piece before its animation completes",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    LegendsProgress now;
    now.shards = 1;
    value.begin({}, now);
    value.start({}, {});
    ticks(value, 93);
    CHECK(value.displayedProgress().shards == 0);
    ticks(value, 1);
    CHECK(value.gesture() == "SHD1");
    ticks(value, 44);
    CHECK(value.displayedProgress().shards == 0);
    ticks(value, 1);
    CHECK(value.displayedProgress().shards == 1);
    CHECK(value.shardOpacity() == 0);
    ticks(value, 22 + 26);
    CHECK(value.gesture() == "SHDR");
    CHECK(value.shardOpacity() == Catch::Approx(47.0f / 255));
    ticks(value, 120);
    CHECK_FALSE(value.active());
}

TEST_CASE("N64 shard pane fades in through the native wrapped transparency bytes",
          "[legends][tower-ceremony]") {
    // Without its overlay a ceremony keeps the script's clocks; only the captions and the
    // camera's focus come from Tower.bin.
    LegendsTowerCeremony value;
    value.bindSequences(
        {{"REDY", 50, 1.0f / 30, true}, {"SHD1", 45, 1.0f / 30}, {"SHDR", 30, 1.0f / 30}});
    LegendsProgress now;
    now.shards = 1;
    value.begin({}, now);
    value.start({}, {});
    ticks(value, 94 + 45); // SHD1 starts on tick 94; its end is read 45 updates on
    REQUIRE(value.displayedProgress().shards == 1);
    CHECK(value.takeSounds().empty());
    // e0863440 case 5 shows case 4's clear white bytes for 22 updates, playing 0x68 on
    // the last, which still shows them.
    for (s32 tick = 1; tick <= 22; ++tick) {
        CAPTURE(tick);
        ticks(value, 1);
        CHECK(value.takeSounds() == (tick == 22 ? std::vector<u32>{0x68} : std::vector<u32>{}));
        CHECK(value.shardOpacity() == 0);
        CHECK(value.shardWhiteBlend() == 1);
    }
    // Case 6's counts: opacity is 255 less a transparency that wraps for counts 1 to 3.
    constexpr std::array<std::array<s32, 3>, 8> kCounts{{{1, 30, 250},
                                                         {2, 19, 253},
                                                         {3, 9, 255},
                                                         {4, 255, 255},
                                                         {5, 245, 255},
                                                         {10, 193, 233},
                                                         {24, 47, 0},
                                                         {25, 47, 0}}};
    usize row = 0;
    for (s32 count = 1; count <= 25; ++count) {
        CAPTURE(count);
        ticks(value, 1);
        CHECK(value.gesture() == "SHD1");
        if (count == 4) {
            CHECK(value.shardOpacity() == 1);
        }
        if (row < kCounts.size() && kCounts[row][0] == count) {
            CHECK(value.shardOpacity() == Catch::Approx(static_cast<f32>(kCounts[row][1]) / 255));
            CHECK(value.shardWhiteBlend() ==
                  Catch::Approx(static_cast<f32>(kCounts[row][2]) / 255));
            ++row;
        }
    }
    CHECK(row == kCounts.size());
    // Case 7 plays SHDR on the update after the pane settles, and the pane holds.
    ticks(value, 1);
    CHECK(value.gesture() == "SHDR");
    CHECK(value.shardOpacity() == Catch::Approx(47.0f / 255));
    CHECK(value.shardWhiteBlend() == 0);
}

TEST_CASE("N64 last shard queues the completed window's caption pair after its own",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    LegendsProgress old;
    old.shards = 7;
    LegendsProgress now = old;
    now.shards = 15;
    value.begin(old, now);
    value.start({}, {});
    std::vector<u32> shown;
    for (s32 i = 0; i < 2000 && value.active(); ++i) {
        if (const auto caption = value.caption();
            caption && (shown.empty() || shown.back() != *caption)) {
            shown.push_back(*caption);
        }
        value.update(1.0f / 30);
    }
    CHECK_FALSE(value.active());
    CHECK(shown == std::vector<u32>{535, 536, 539, 540});
}

TEST_CASE("N64 ceremony captions run e086098c's clock on script and return updates",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    value.bindSequences(nativeSequences());
    LegendsProgress old;
    old.shards = 7;
    LegendsProgress now = old;
    now.shards = 15;
    value.begin(old, now);
    value.start({}, {});
    CHECK_FALSE(value.caption());
    // Each caption is removed on its 91st update and the next shows from the update after;
    // 540's updates skip the Select update, 338.
    std::vector<std::pair<s32, std::optional<u32>>> changes;
    std::optional<u32> showing;
    for (s32 tick = 1; tick <= 394; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        if (value.caption() != showing) {
            showing = value.caption();
            changes.emplace_back(tick, showing);
        }
        // The float32 fades of 535's first and last updates.
        constexpr std::array<std::pair<s32, s32>, 7> kOpacity{
            {{2, 29}, {9, 227}, {10, 255}, {83, 255}, {84, 199}, {90, 29}, {91, 1}}};
        for (const auto& [at, opacity] : kOpacity) {
            if (tick == at) {
                CHECK(value.captionOpacity() == Catch::Approx(static_cast<f32>(opacity) / 255));
            }
        }
    }
    CHECK_FALSE(value.active());
    const std::vector<std::pair<s32, std::optional<u32>>> expected{
        {2, 535},   {92, std::nullopt},  {93, 536},  {183, std::nullopt},
        {184, 539}, {274, std::nullopt}, {275, 540}, {366, std::nullopt}};
    CHECK(changes == expected);
}

TEST_CASE("N64 gate animation holds the staff spell before releasing the actual door",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    LegendsProgress now;
    now.obelisks = 7;
    value.begin({}, now);
    value.start({}, {});
    ticks(value, 48);
    CHECK(value.gesture() == "STRT");
    CHECK(value.gestureFrame() == 44);
    ticks(value, 44);
    CHECK(value.gestureFrame() == 0);
    ticks(value, 46);
    CHECK(value.gesture() == "ODOR");
    ticks(value, 15);
    CHECK(value.gestureFrame() == 15);
    ticks(value, 49);
    CHECK(value.gestureFrame() == 15);
    CHECK(value.displayedProgress().obelisks == 0);
    ticks(value, 7);
    CHECK(value.displayedProgress().obelisks == 0);
    ticks(value, 1);
    CHECK(value.displayedProgress().obelisks == 7);
    ticks(value, 120);
    CHECK_FALSE(value.active());
}

TEST_CASE("N64 ceremony clocks ignore invalid steps and are independent of render rate",
          "[legends][tower-ceremony]") {
    LegendsProgress now;
    now.runestones = 1;
    now.obelisks = 7;
    now.shards = 1;
    auto a = nativeCeremony();
    a.begin({}, now);
    auto b = a;
    auto c = a;
    for (auto* value : {&a, &b, &c}) {
        value->start({}, {});
    }
    a.update(-1);
    a.update(0);
    a.update(std::numeric_limits<f32>::quiet_NaN());
    a.update(std::numeric_limits<f32>::infinity());
    CHECK(a.gestureFrame() == 0);
    CHECK(a.takeSounds().empty());
    // Two 1/60 s and four 1/120 s steps make exactly one 30 Hz tick each time.
    s32 tick = 0;
    while (a.active() && tick < 1000) {
        ++tick;
        CAPTURE(tick);
        a.update(1.0f / 30);
        for (s32 i = 0; i < 2; ++i) {
            b.update(1.0f / 60);
        }
        for (s32 i = 0; i < 4; ++i) {
            c.update(1.0f / 120);
        }
        const auto heard = a.takeSounds();
        for (auto* other : {&b, &c}) {
            CHECK(other->takeSounds() == heard);
            CHECK(other->gesture() == a.gesture());
            CHECK(other->gestureFrame() == a.gestureFrame());
            CHECK(other->displayedProgress() == a.displayedProgress());
            CHECK(other->camera() == a.camera());
            CHECK(other->active() == a.active());
        }
    }
    CHECK(tick == 736);
    CHECK_FALSE(b.active());
    CHECK_FALSE(c.active());
}
