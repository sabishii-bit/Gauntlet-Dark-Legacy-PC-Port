#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/math/PortableMath.h"

#include "TestSupport.h"
#include "expansions/n64/level/LegendsTowerCeremony.h"
#include "expansions/n64/level/LegendsTowerProgress.h"

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

/** A built view against the eye and angles the channels' float32 arithmetic gives, within
 * the trig's last bits. */
void checkView(const WorldCamera& camera, const Vec3& eye, f32 pitch, f32 yaw) {
    CHECK(glm::distance(camera.position, eye) < 1e-4f);
    CHECK(camera.pitch == Catch::Approx(pitch).margin(1e-5f));
    CHECK(camera.yaw == Catch::Approx(yaw).margin(1e-5f));
    CHECK(camera.roll == 0);
}

/** The yaw Tower.bin's turn faces Sumner at `sumner` toward `target`: minus the arc tangent of
 * its x and z offsets. */
f32 sumnerAngle(const Vec3& sumner, const Vec3& target) {
    return -portable::atan2(target.x - sumner.x, target.z - sumner.z);
}

/** A point before Sumner at the origin facing `yaw`, along (-sin, cos) as his podium is. */
Vec3 before(f32 yaw) {
    return {-portable::sin(yaw), 0, portable::cos(yaw)};
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

TEST_CASE("N64 Tower setup presents one new piece of each kind and leaves the rest for later",
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
    // e0862220 takes the lowest new stone, captioned by e086aec8's one seen stone; e08653bc
    // takes the Castle door; e0863364 takes the highest new shard.
    REQUIRE(value.entries().size() == 3);
    const auto& rune = value.entries()[0];
    CHECK(rune.kind == LegendsTowerCeremony::Kind::Rune);
    CHECK(rune.index == 1);
    CHECK(rune.captions == std::vector<u32>{542});
    CHECK(rune.completesRunes);
    const auto& door = value.entries()[1];
    CHECK(door.kind == LegendsTowerCeremony::Kind::Door);
    CHECK(door.index == 1);
    CHECK(door.captions == std::vector<u32>{523, 524});
    const auto& shard = value.entries()[2];
    CHECK(shard.kind == LegendsTowerCeremony::Kind::Shard);
    CHECK(shard.index == 3);
    CHECK(shard.captions == std::vector<u32>{535, 536});
    CHECK(shard.completesWindow);
    CHECK(value.displayedProgress().runestones == old.runestones);
    CHECK(value.displayedProgress().shards == old.shards);
    // e0864e7c builds the Town, Ice and Battlefield doors already open.
    CHECK(LegendsTowerProgress::of(value.displayedProgress()).doors ==
          std::array{true, false, true, true});
    CHECK(now.runestones == 0x1fff); // Presentation does not own/save the award.
    value.start({}, {}, {});
    std::vector<u32> shown;
    for (s32 i = 0; i < 3000 && value.active(); ++i) {
        if (const auto caption = value.caption();
            caption && (shown.empty() || shown.back() != *caption)) {
            shown.push_back(*caption);
        }
        value.update(kTick);
    }
    CHECK_FALSE(value.active());
    // Each script runs once; the shard that makes the window still turns to it.
    CHECK(shown == std::vector<u32>{542, 523, 524, 535, 536, 539, 540});
    // The other eleven stones and two panes stay undrawn for the rest of the visit, while the
    // window and the rune portal show the completions.
    CHECK(value.displayedProgress().runestones == 3);
    CHECK(value.displayedProgress().shards == 9);
    CHECK(value.displayedProgress().obelisks == 255);
    const auto completed = LegendsTowerProgress::of(value.completed(now));
    CHECK(completed.cathedralBeam);
    CHECK(completed.runestonePortal);
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
    value.start(original, {0, 0, 0}, {0, 0, 0});
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
    // e007df20 rebuilds the gameplay view about the party, within its last thousandths.
    const auto back = value.camera();
    CHECK(glm::distance(back.position, original.position) < .05f);
    CHECK(back.pitch == Catch::Approx(original.pitch).margin(.001f));
    CHECK(back.yaw == Catch::Approx(original.yaw).margin(.001f));
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
    value.start({}, {}, {});
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

TEST_CASE("N64 REDY after MKRU plays once and the ready loop wraps without e0860d1c",
          "[legends][tower-ceremony]") {
    // Only an update that runs no e0860d1c tells them apart: the rune's script returns on
    // 175, so 176 selects nothing and 177 plans the return. Frames on 175 to 178:
    const auto readyFrames = [](s32 frames, bool repeats) {
        auto value = nativeCeremony({{"REDY", frames, 1.0f / 30, repeats}});
        LegendsProgress now;
        now.runestones = 1;
        value.begin({}, now);
        value.start({}, {}, {});
        ticks(value, 174);
        std::array<f32, 4> shown{};
        for (auto& frame : shown) {
            value.update(kTick);
            frame = value.gestureFrame();
        }
        return shown;
    };
    // The REDY that MKRU's end starts on 111 keeps e0860ca4's once flag: 65 frames freeze at
    // the last on 175 and stay through 176, until e0860d1c restarts REDY looping on 177.
    CHECK(readyFrames(65, true) == std::array<f32, 4>{64, 64, 0, 1});
    // The loop e0860d1c restarts wraps in every update's pass; bound to play once, it
    // freezes at its last frame until e0860d1c runs.
    CHECK(readyFrames(5, true) == std::array<f32, 4>{4, 0, 1, 2});
    CHECK(readyFrames(5, false) == std::array<f32, 4>{4, 4, 0, 1});
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
    value.start({}, {}, {});
    for (s32 tick = 1; tick <= 202; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        CHECK(value.displayedProgress().runestones == (tick >= 144 ? 0x1fffU : 0x1ffeU));
        CHECK(value.active() == (tick < 202));
    }
}

TEST_CASE("N64 Tower return places only the lowest new runestone and leaves the other undrawn",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress before;
    before.runestones = 1;
    LegendsProgress after;
    after.runestones = 0b111;
    value.begin(before, after);
    // e0862220 breaks at the lowest new stone; the dispatcher runs its script once.
    REQUIRE(value.entries().size() == 1);
    CHECK(value.entries()[0].kind == LegendsTowerCeremony::Kind::Rune);
    CHECK(value.entries()[0].index == 1);
    CHECK_FALSE(value.entries()[0].completesRunes);
    value.start({}, {}, {});
    for (s32 tick = 1; tick <= 232; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        // Stone 1 keeps a lone stone's clock; e0861630 drew only the seen stone 0.
        CHECK(value.displayedProgress().runestones == (tick >= 144 ? 0b011U : 0b001U));
        CHECK(value.active() == (tick < 232));
    }
    // Stone 2 stays undrawn for the rest of the visit; a stone gained later shows at once.
    CHECK(value.displayed(after).runestones == 0b011);
    LegendsProgress later = after;
    later.runestones |= 1U << 5;
    CHECK(value.displayed(later).runestones == 0b100011);
}

TEST_CASE("N64 Tower ceremony views carry the party's Tower pieces and none of its records",
          "[legends][tower-ceremony]") {
    // The adapter takes these views on every update, so they copy only what the Tower draws.
    LegendsProgress before;
    before.runestones = 1;
    LegendsProgress after = before;
    after.runestones = 0b11;
    after.shards = 0b10;
    after.obelisks = 7;
    after.legendItems = 5;
    REQUIRE(after.recordCompleted("mount6"));
    auto value = nativeCeremony();
    value.begin(before, after);
    REQUIRE(value.pending());
    const auto pieces = LegendsTowerCeremony::pieces(after);
    CHECK(pieces.runestones == after.runestones);
    CHECK(pieces.shards == after.shards);
    CHECK(pieces.obelisks == after.obelisks);
    // The new stone, pane and Castle door wait for their scripts; no completion waits.
    const auto displayed = value.displayed(after);
    CHECK(displayed.runestones == 1);
    CHECK(displayed.shards == 0);
    CHECK(displayed.obelisks == 0);
    CHECK(value.displayedProgress() == displayed);
    CHECK(value.completed(after) == pieces);
    for (const auto& view : {pieces, displayed, value.completed(after)}) {
        CHECK(view.legendItems == 0);
        CHECK(view.completedMaps.empty());
    }
}

TEST_CASE("N64 thirteen stones made by two new ones open the rune portal as the first settles",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress before;
    before.runestones = 0x1ffc;
    LegendsProgress after;
    after.runestones = 0x1fff;
    value.begin(before, after);
    // e0862220 sets e086af00 from the seen and new stones together.
    REQUIRE(value.entries().size() == 1);
    CHECK(value.entries()[0].index == 0);
    CHECK(value.entries()[0].completesRunes);
    CHECK_FALSE(LegendsTowerProgress::of(value.completed(after)).runestonePortal);
    value.start({}, {}, {});
    for (s32 tick = 1; tick <= 202; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        // Stone 0 lands on 144 and its script returns at its settle stamp on 145, where
        // e086239c turns the portal on (e086aec0); stone 1 stays undrawn.
        CHECK(value.displayedProgress().runestones == (tick >= 144 ? 0x1ffdU : 0x1ffcU));
        CHECK(LegendsTowerProgress::of(value.completed(after)).runestonePortal == (tick >= 145));
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
        value.start({}, {}, {});
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
        value.start({}, {}, {});
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
    value.start({}, {}, {});
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
        value.start({}, {}, {});
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
        value.start({}, {}, {});
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

TEST_CASE("N64 Tower return opens only the new door e08653bc ranks first",
          "[legends][tower-ceremony]") {
    // Doors stand Town, Castle, Ice, Battlefield, wanting obelisks 0x60, 0x07, 0x18 and 0x80;
    // e08653bc ranks Castle, Battlefield, Ice, then Town.
    struct Return {
        u64 before;
        u64 after;
        usize selected;
        std::array<bool, 4> open; ///< the doors e0864e7c builds open at the setup
    };
    constexpr std::array<Return, 5> kReturns{{{0, 0xff, 1, {true, false, true, true}},
                                              {0, 0xf8, 3, {true, false, true, false}},
                                              {0, 0x78, 2, {true, false, false, false}},
                                              {0, 0x60, 0, {false, false, false, false}},
                                              {0x01, 0x87, 1, {false, false, false, true}}}};
    for (const auto& visit : kReturns) {
        CAPTURE(visit.after);
        auto value = nativeCeremony();
        LegendsProgress before;
        before.obelisks = visit.before;
        LegendsProgress after;
        after.obelisks = visit.after;
        value.begin(before, after);
        // e08653bc's last assignment wins; the doors it does not select get no script.
        REQUIRE(value.entries().size() == 1);
        CHECK(value.entries()[0].kind == LegendsTowerCeremony::Kind::Door);
        CHECK(value.entries()[0].index == visit.selected);
        CHECK(LegendsTowerProgress::of(value.displayedProgress()).doors == visit.open);
    }
    // The Castle door opens on a lone door's clock, the other three open throughout.
    auto value = nativeCeremony();
    LegendsProgress after;
    after.obelisks = 0xff;
    value.begin({}, after);
    value.start({}, {}, {});
    for (s32 tick = 1; tick <= 358; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        CHECK(value.displayedProgress().obelisks == (tick >= 259 ? 0xffU : 0xf8U));
        CHECK(value.active() == (tick < 358));
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
            value.start({}, {}, {});
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
        value.start({}, {}, {});
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
    value.start({}, {}, {});
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
    value.start({}, {}, {});
    CHECK_FALSE(value.cathedralBeamShown());
    CHECK(value.cathedralBeamAlpha() == 0);
    for (s32 tick = 1; tick <= 394; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        // Case 7 (161) unhides the beam fully transparent; case 8 hands over on its 52nd
        // update, and case 9's counts 0 to 40 run from 214.
        const s32 count = std::clamp(tick - 214, 0, 40);
        const s32 transparency = (40 - count) * 255 / 40;
        CHECK(value.windowReveal() == Catch::Approx(static_cast<f32>(count) / 40));
        CHECK(value.cathedralBeamShown() == (tick >= 161));
        CHECK(value.cathedralBeamAlpha() ==
              Catch::Approx(static_cast<f32>(255 - transparency) / 255));
        CHECK(value.active() == (tick < 394));
    }
    CHECK(value.cathedralBeamAlpha() == 1);
    // e0862fc4 leaves a completed window's beam shown through later ceremonies.
    const LegendsProgress window = after;
    LegendsProgress rune = window;
    rune.runestones = 1;
    value.begin(window, rune);
    value.start({}, {}, {});
    for (s32 tick = 1; tick <= 232; ++tick) {
        CAPTURE(tick);
        CHECK(value.cathedralBeamShown());
        CHECK(value.cathedralBeamAlpha() == 1);
        value.update(kTick);
    }
    CHECK_FALSE(value.active());
}

TEST_CASE("N64 Tower return installs only the highest new shard and keeps the other pane hidden",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress after;
    after.shards = 0b0101;
    value.begin({}, after);
    // e0863364 keeps the highest new shard; the dispatcher runs its script once.
    REQUIRE(value.entries().size() == 1);
    CHECK(value.entries()[0].kind == LegendsTowerCeremony::Kind::Shard);
    CHECK(value.entries()[0].index == 2);
    CHECK_FALSE(value.entries()[0].completesWindow);
    value.start({}, {}, {});
    for (s32 tick = 1; tick <= 260; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        // Case 4 unhides only the selected pane, on a lone shard's clock.
        CHECK(value.displayedProgress().shards == (tick >= 113 ? 0b0100U : 0U));
        CHECK(value.active() == (tick < 260));
    }
    CHECK(value.displayed(after).shards == 0b0100);
}

TEST_CASE("N64 window made by two new shards still turns to its reveal",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress before;
    before.shards = 0b0011;
    LegendsProgress after;
    after.shards = 0b1111;
    value.begin(before, after);
    // e0863364 sets e086ae04 from the seen and new shards together.
    REQUIRE(value.entries().size() == 1);
    CHECK(value.entries()[0].index == 3);
    CHECK(value.entries()[0].completesWindow);
    value.start({}, {}, {});
    for (s32 tick = 1; tick <= 394; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        // A fourth shard's counts: case 7 (161) unhides the beam and case 9 fades it from 214.
        const s32 count = std::clamp(tick - 214, 0, 40);
        const s32 transparency = (40 - count) * 255 / 40;
        CHECK(value.windowReveal() == Catch::Approx(static_cast<f32>(count) / 40));
        CHECK(value.cathedralBeamShown() == (tick >= 161));
        CHECK(value.cathedralBeamAlpha() ==
              Catch::Approx(static_cast<f32>(255 - transparency) / 255));
        // Pane 2 stays hidden, but the window's completion shows from the turn to it.
        CHECK(value.displayedProgress().shards == (tick >= 113 ? 0b1011U : 0b0011U));
        CHECK(LegendsTowerProgress::of(value.completed(after)).cathedralBeam == (tick >= 161));
        CHECK(value.active() == (tick < 394));
    }
}

TEST_CASE("N64 ceremonies select each entry on the update after the last one returns",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    LegendsProgress now;
    now.runestones = 1;
    now.shards = 1;
    value.begin({}, now);
    REQUIRE(value.entries().size() == 2);
    value.start({}, {}, {});
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
    value.start({}, {}, {});
    for (s32 tick = 1; tick <= 736; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        CHECK(value.displayedProgress().obelisks == (tick >= 175 + 259 ? 7U : 0U));
        CHECK(value.active() == (tick < 736));
    }
}

TEST_CASE("N64 ceremony camera orbits the party's midpoint with Sumner and returns in 54",
          "[legends][tower-ceremony]") {
    // Without the overlay Sumner and the shard point stand at the origin. The views are
    // game.bin's channels (e007cdf0, e007d774) and e007db00's build worked through in float32.
    auto value = nativeCeremony();
    LegendsProgress now;
    now.shards = 1;
    value.begin({}, now);
    WorldCamera original;
    original.position = {20, 30, 40};
    original.pitch = .4f;
    original.yaw = .8f;
    const Vec3 attention{1, 2, 3};
    const Vec3 party{3, 0, 12};
    value.start(original, attention, party);
    // Case 0 installs no view, and the shard's Select (e0863410) does not step the camera:
    // the gameplay view stays until the script's first update builds the cut.
    CHECK(value.camera() == original);
    CHECK(value.attention() == attention);
    const Vec3 centre = party * .5f;
    WorldCamera previous = original;
    for (s32 tick = 1; tick <= 260; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        const auto camera = value.camera();
        // The shard shot, planned on 43, moves over .7, 1 and .3 s: 60 updates from 44. The
        // script returns on 203; Select (204) and case 0x28's planning update (205) hold the
        // camera, and the return ends on its 54th step.
        CHECK((camera != previous) ==
              (tick == 2 || (tick >= 44 && tick <= 103) || (tick >= 206 && tick <= 259)));
        if (tick == 2) {
            // e0860620's (30, pi/6, pi/3) about the midpoint, looking over Sumner, unblended.
            CHECK(value.camera(0) == camera);
            CHECK(value.camera(.5f) == camera);
            checkView(camera, {-21, 15, -6.99038029f}, .49681893f, 1.24945807f);
            CHECK(value.attention() == Vec3{0, 3, 0});
        }
        if (tick >= 44 && tick <= 103) {
            // The orbit's angles move, not the eye along a line: it keeps 30 from the centre,
            // looking at the moving attention.
            CHECK(glm::distance(camera.position, centre) == Catch::Approx(30).margin(.001f));
            CHECK(glm::distance(camera.forward(),
                                glm::normalize(value.attention() - camera.position)) < 1e-5f);
        }
        if (tick == 73) {
            checkView(camera, {-24.4095802f, 10.9137983f, 16.4681683f}, .234910622f, 2.16431236f);
        }
        if (tick == 103) {
            checkView(camera, {1.50000262f, 5.20944595f, 35.5442314f}, .00588723645f, -3.09941673f);
            CHECK(value.attention() == Vec3{0, 5, 0});
        }
        if (tick == 259) {
            // e007df20 rebuilds the eye about the party; the orbit's last remainders fall under
            // .001 an update early, leaving it a little short of the gameplay eye.
            checkView(camera, {19.9913311f, 29.9754162f, 40.0315704f}, .4f, .8f);
            CHECK(glm::distance(camera.position, original.position) < .05f);
            CHECK(value.attention() == attention);
        }
        CHECK(value.active() == (tick < 260));
        previous = camera;
    }
}

TEST_CASE("N64 ceremony's cut shows on the update a script first steps the camera",
          "[legends][tower-ceremony]") {
    // e086239c(1, 0) and e0865c98(1, dt) step the camera on the Select update; e0863410 does
    // not, so a shard's cut waits for its script's first update.
    WorldCamera original;
    original.position = {20, 30, 40};
    original.pitch = .4f;
    original.yaw = .8f;
    LegendsProgress rune;
    rune.runestones = 1;
    LegendsProgress door;
    door.obelisks = 7;
    LegendsProgress shard;
    shard.shards = 1;
    for (const auto& [now, cutTick] :
         {std::pair{rune, 1}, std::pair{door, 1}, std::pair{shard, 2}}) {
        CAPTURE(cutTick);
        auto value = nativeCeremony();
        value.begin({}, now);
        value.start(original, {1, 2, 3}, {3, 0, 12});
        for (s32 tick = 1; tick <= 3; ++tick) {
            CAPTURE(tick);
            value.update(kTick);
            CHECK((value.camera() == original) == (tick < cutTick));
            if (tick == cutTick) {
                CHECK(value.camera(.5f) == value.camera());
            }
        }
        checkView(value.camera(), {-21, 15, -6.99038029f}, .49681893f, 1.24945807f);
    }
}

TEST_CASE("N64 ceremony shots orbit the party's midpoint with Sumner at Tower.bin's angles",
          "[legends][tower-ceremony][assets]") {
    // A party at tower2's entrance. Sumner stands at e086a678 and the shots are e0860620's,
    // e086239c's, e0865c98's (yaws from e08696c4) and e0863440's; the views are game.bin's
    // channels and e007db00's build worked through in float32. The Ice door's and the
    // window's orbits change in pitch alone, and their last thousandth falls short as retail's.
    const Vec3 party{-4.5f, 0, -15.5f};
    WorldCamera original;
    original.position = {20, 30, 40};
    original.pitch = .4f;
    original.yaw = .8f;
    {
        auto value = ceremony();
        LegendsProgress now;
        now.runestones = 1;
        value.begin({}, now);
        value.start(original, {}, party);
        ticks(value, 1);
        checkView(value.camera(), {-21.75f, 15, -24.7403793f}, .3546215f, 1.02798545f);
    }
    // The settled shot: the camera when the script returns.
    const auto settled = [&](const LegendsProgress& before, const LegendsProgress& after) {
        auto value = ceremony();
        value.begin(before, after);
        value.start(original, {}, party);
        for (s32 tick = 0; tick < 2000 && value.current() != nullptr; ++tick) {
            value.update(kTick);
        }
        return value.camera();
    };
    LegendsProgress now;
    now.runestones = 1;
    checkView(settled({}, now), {.75f, 12.6785479f, -38.9392319f}, .144025818f, .0141662396f);
    // Doors in LegendsTowerProgress order: Town, Castle, Ice and Battlefield.
    struct Shot {
        Vec3 eye;
        f32 pitch;
        f32 yaw;
    };
    const std::array<Shot, 4> doors{
        {{{24.2965679f, 12.6785488f, -25.3446159f}, .0445852242f, -.913980901f},
         {{-16.7268982f, 12.6785488f, 9.07816505f}, .084050633f, 2.08375835f},
         {{-22.7859135f, 12.7048979f, -25.3384647f}, .0513792336f, .907872558f},
         {{.75f, 12.6785488f, -38.9392319f}, .138238087f, -.00893479772f}}};
    for (usize i = 0; i < doors.size(); ++i) {
        CAPTURE(i);
        LegendsProgress opened;
        opened.obelisks = LegendsTowerProgress::kDoorObelisks[i];
        checkView(settled({}, opened), doors[i].eye, doors[i].pitch, doors[i].yaw);
    }
    LegendsProgress shard;
    shard.shards = 1;
    checkView(settled({}, shard), {.750002563f, 5.20944595f, 17.7942314f}, -.157120496f,
              -3.12945604f);
    LegendsProgress seen;
    seen.shards = 7;
    LegendsProgress window;
    window.shards = 15;
    checkView(settled(seen, window), {.750002861f, 18.9858017f, 21.16716f}, .166997686f,
              -3.13008428f);
}

TEST_CASE("N64 ceremony's cut turns each arrived player to face Sumner in the native yaw",
          "[legends][tower-ceremony]") {
    // e0860620 hands e0010e50 Sumner's x and z offsets from each player, the arc tangent of
    // x over z: a player due south of him (less z) faces him at yaw 0, and height plays no
    // part. Without the overlay Sumner stands at the origin.
    const auto value = nativeCeremony();
    CHECK(value.partyFacing({0, 0, -10}) == 0);
    CHECK(value.partyFacing({0, 7, 10}) == Catch::Approx(kPi));
    CHECK(value.partyFacing({10, -2, 0}) == Catch::Approx(-kPi / 2));
    const f32 yaw = value.partyFacing({-3, 1, 4});
    CHECK(yaw == portable::atan2(3.0f, -4.0f));
    // The actors face (sin, cos) of their yaw: toward him.
    CHECK(portable::sin(yaw) == Catch::Approx(.6f));
    CHECK(portable::cos(yaw) == Catch::Approx(-.8f));
}

TEST_CASE("N64 ceremony's party faces Sumner where Tower.bin stands him",
          "[legends][tower-ceremony][assets]") {
    const auto value = ceremony();
    const auto overlay =
        readFile(test::dataDirectory().parent_path() / "assets/n64/extracted/_program/Tower.bin");
    // e0860198 stands Sumner at e086a678 (through e002fbfc), where e0860620 reads him.
    const auto coordinate = [&](usize at) { return std::bit_cast<f32>(readU32BE(overlay, at)); };
    const Vec3 sumner{coordinate(0xa678), coordinate(0xa67c), coordinate(0xa680)};
    for (const Vec3& player : {Vec3{0}, sumner + Vec3{3, 9, -4}, sumner - Vec3{6, 0, 0}}) {
        CHECK(value.partyFacing(player) ==
              portable::atan2(sumner.x - player.x, sumner.z - player.z));
    }
    CHECK(value.partyFacing(sumner + Vec3{0, 0, -6}) == 0);
}

TEST_CASE("N64 shard gesture cannot install the piece before its animation completes",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    LegendsProgress now;
    now.shards = 1;
    value.begin({}, now);
    value.start({}, {}, {});
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
    value.start({}, {}, {});
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
    value.start({}, {}, {});
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
    value.start({}, {}, {});
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

TEST_CASE("N64 door ceremony captions run e086098c's clock from the select update",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    value.bindSequences(nativeSequences());
    LegendsProgress now;
    now.obelisks = 7;
    value.begin({}, now);
    value.start({}, {}, {});
    // e0865c98 queues the door's pair in the select update (tick 1); each caption shows from
    // its first caption update and goes on its 91st.
    std::vector<std::pair<s32, std::optional<u32>>> changes;
    std::optional<u32> showing;
    for (s32 tick = 1; tick <= 358; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        if (value.caption() != showing) {
            showing = value.caption();
            changes.emplace_back(tick, showing);
        }
    }
    CHECK_FALSE(value.active());
    const std::vector<std::pair<s32, std::optional<u32>>> expected{
        {2, 523}, {92, std::nullopt}, {93, 524}, {183, std::nullopt}};
    CHECK(changes == expected);
}

TEST_CASE("N64 gate animation holds the staff spell before releasing the actual door",
          "[legends][tower-ceremony][assets]") {
    auto value = ceremony();
    LegendsProgress now;
    now.obelisks = 7;
    value.begin({}, now);
    value.start({}, {}, {});
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
        value->start({}, {}, {});
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
            CHECK(other->sumnerYaw() == a.sumnerYaw());
        }
    }
    CHECK(tick == 736);
    CHECK_FALSE(b.active());
    CHECK_FALSE(c.active());
    // The staff's shrink after the hand-back runs on the same updates.
    s32 shrinking = 0;
    while (a.presenting() && shrinking < 100) {
        ++shrinking;
        CAPTURE(shrinking);
        a.update(1.0f / 30);
        for (s32 i = 0; i < 2; ++i) {
            b.update(1.0f / 60);
        }
        for (s32 i = 0; i < 4; ++i) {
            c.update(1.0f / 120);
        }
        for (auto* other : {&b, &c}) {
            CHECK(other->staffVisible() == a.staffVisible());
            CHECK(other->staffScale() == a.staffScale());
            CHECK(other->presenting() == a.presenting());
        }
    }
    CHECK(shrinking == 30);
}

TEST_CASE("N64 Sumner turns at most .2 rad an update and wraps once each way past pi",
          "[legends][tower-ceremony]") {
    // Tower.bin e0860ef0..e0861020 in float32, Sumner at the origin.
    using Ceremony = LegendsTowerCeremony;
    const Vec3 sumner{0};
    // A long turn moves .2 (0x3e4ccccd) an update and its last step lands on the angle, where a
    // turn of 0 leaves him.
    const Vec3 target = before(.9f);
    const f32 angle = sumnerAngle(sumner, target);
    f32 yaw = 0;
    for (s32 update = 1; update <= 4; ++update) {
        CAPTURE(update);
        const f32 previous = yaw;
        yaw = Ceremony::turnSumner(yaw, sumner, target);
        CHECK(yaw == previous + .2f);
    }
    yaw = Ceremony::turnSumner(yaw, sumner, target);
    CHECK(yaw == angle);
    CHECK(Ceremony::turnSumner(yaw, sumner, target) == angle);
    // A turn past float pi wraps the short way round, beyond pi rather than back through 0;
    // one short of it does not, and goes the long way.
    CHECK(Ceremony::turnSumner(3, sumner, before(-3)) == 3 + .2f);
    CHECK(Ceremony::turnSumner(-3, sumner, before(3)) == -3 - .2f);
    CHECK(Ceremony::turnSumner(0, sumner, before(3.1f)) == .2f);
    CHECK(Ceremony::turnSumner(0, sumner, before(-3.1f)) == -.2f);
    // It wraps once each way, however far his yaw has run: from 10 toward 0 the turn of -10
    // becomes -10 + 2 pi, still under -pi, and he steps down.
    CHECK(Ceremony::turnSumner(10, sumner, Vec3{0, 0, 1}) == 10 - .2f);
}

TEST_CASE("N64 Sumner turns toward the party until a shot and then toward its focus",
          "[legends][tower-ceremony]") {
    // Without the overlay Sumner stands at the origin at yaw 0 and the scripts' foci stand
    // over him, so a shot turns him back to 0, as his podium does. The cut aims him at the
    // party's mean feet (e0860684) and leaves his yaw; e0860d1c turns him on the script and
    // return updates only, so neither the cut, the Select updates nor the hand-back turn him.
    const Vec3 party{3, 0, -4};
    const f32 towardParty = sumnerAngle(Vec3{0}, party);
    struct Visit {
        std::string_view kind;
        LegendsProgress after;
        s32 shot;     ///< the rune's time 20, the door's and shard's case 2 after their opening
        s32 handBack; ///< the update control returns
    };
    LegendsProgress rune;
    rune.runestones = 1;
    LegendsProgress door;
    door.obelisks = 7;
    LegendsProgress shard;
    shard.shards = 1;
    const std::array<Visit, 3> visits{
        {{"rune", rune, 21, 232}, {"door", door, 42, 358}, {"shard", shard, 43, 260}}};
    for (const auto& visit : visits) {
        CAPTURE(visit.kind);
        auto value = nativeCeremony();
        value.begin({}, visit.after);
        CHECK(value.sumnerYaw() == 0);
        CHECK_FALSE(value.partyFacingTarget());
        value.start({}, {}, party);
        CHECK(value.sumnerYaw() == 0);
        CHECK(value.partyFacingTarget() == Vec3{0});
        f32 expected = 0;
        for (s32 tick = 1; tick <= visit.handBack; ++tick) {
            CAPTURE(tick);
            value.update(kTick);
            if (tick >= 2) {
                expected = LegendsTowerCeremony::turnSumner(
                    expected, Vec3{0}, tick < visit.shot ? party : Vec3{0, 5, 0});
            }
            CHECK(value.sumnerYaw() == expected);
            // Twelve updates of .2 and the rest bring him round to the party on update 14, and
            // the shot's turn back to 0 lands as many updates after it.
            if (tick == 1) {
                CHECK(value.sumnerYaw() == 0);
            }
            if (tick == 14 || tick == visit.shot - 1) {
                CHECK(value.sumnerYaw() == towardParty);
            }
            if (tick == visit.shot) {
                CHECK(value.sumnerYaw() == towardParty + .2f);
            }
            if (tick >= visit.shot + 12) {
                CHECK(value.sumnerYaw() == 0);
            }
            // e086adb0: Sumner's feet from the cut, the focus from the shot, none once the
            // party is let go.
            CHECK(value.partyFacingTarget() == (tick == visit.handBack ? std::nullopt
                                                : tick < visit.shot
                                                    ? std::optional{Vec3{0}}
                                                    : std::optional{Vec3{0, 5, 0}}));
        }
    }
}

TEST_CASE("N64 ceremony holds the party from its cut and gives back each heading at its hand-back",
          "[legends][tower-ceremony]") {
    auto value = nativeCeremony();
    const Vec3 feet{-3, 0, 4};
    // Nothing is held before a ceremony is queued.
    CHECK_FALSE(value.hold(0, feet, .5f));
    LegendsProgress now;
    now.runestones = 1;
    value.begin({}, now);
    // Queued, a seat's first hold keeps the heading it stands at before the cut turns it, its
    // record's +0xa8; the holds after it, the party turned to Sumner already, keep that one.
    CHECK_FALSE(value.hold(0, feet, .5f));
    CHECK_FALSE(value.hold(0, feet, 2));
    value.start({}, {}, feet);
    // From the cut each held seat faces Sumner (e0048acc, then e0860d1c's matrix), and one first
    // held now keeps the heading it stands at now. The native four seats are all it holds.
    const f32 towardSumner = value.partyFacing(feet);
    CHECK(value.hold(0, feet, 2) == towardSumner);
    CHECK(value.hold(1, feet, 1.25f) == towardSumner);
    CHECK(value.hold(2, feet, .75f) == towardSumner);
    CHECK_FALSE(value.hold(LegendsTowerCeremony::kSeats, feet, 1));
    // Without the overlay every focus stands over Sumner, so its angle from the feet is his.
    for (s32 tick = 1; tick < 232; ++tick) {
        CAPTURE(tick);
        value.update(kTick);
        CHECK(value.hold(0, feet, 3) == towardSumner);
        CHECK(value.hold(1, feet, 3) == towardSumner);
    }
    // The hand-back's update (e0860154) gives each seat back the heading it kept, once: there
    // game.bin's e0048b2c turns a standing player whose yaw is not +0xa8 back to it.
    value.update(kTick);
    REQUIRE_FALSE(value.active());
    CHECK(value.hold(0, feet, 3) == .5f);
    CHECK(value.hold(1, feet, 3) == 1.25f);
    CHECK_FALSE(value.hold(0, feet, 3));
    CHECK_FALSE(value.hold(1, feet, 3));
    CHECK_FALSE(value.hold(3, feet, 3));
    // A new queue holds the party afresh, dropping a heading no hand-back gave back.
    value.begin({}, {});
    CHECK_FALSE(value.hold(2, feet, 3));
}

TEST_CASE("N64 Sumner turns to the party and Tower.bin's foci and back onto his podium",
          "[legends][tower-ceremony][assets]") {
    // Sumner stands at e086a678 at e0860198's yaw 0x40278d37, the party at tower2's entrance.
    // The angles are Tower.bin's turn worked through in float32 apart from this code, within
    // the arc tangent's last bits.
    const Vec3 party{-4.5f, 0, -15.5f};
    const auto rest = std::bit_cast<f32>(0x40278d37U);
    constexpr f32 kTowardParty = 2.19104576f;
    constexpr f32 kTowardCircle = .201762348f;
    constexpr f32 kAngleBits = 1e-6f;
    {
        auto value = ceremony();
        LegendsProgress now;
        now.runestones = 1;
        value.begin({}, now);
        CHECK(value.sumnerYaw() == rest);
        value.start({}, {}, party);
        ticks(value, 1);
        CHECK(value.sumnerYaw() == rest);
        // Updates 2 and 3 turn .2 each, and 4 lands on the party.
        ticks(value, 2);
        CHECK(value.sumnerYaw() == rest - .2f - .2f);
        ticks(value, 1);
        const f32 towardParty = value.sumnerYaw();
        CHECK(towardParty == Catch::Approx(kTowardParty).margin(kAngleBits));
        ticks(value, 16);
        CHECK(value.sumnerYaw() == towardParty);
        // The rune's shot on 21 turns him -114 degrees to the stone circle by 30.
        ticks(value, 1);
        CHECK(value.sumnerYaw() == towardParty - .2f);
        ticks(value, 9);
        const f32 towardCircle = value.sumnerYaw();
        CHECK(towardCircle == Catch::Approx(kTowardCircle).margin(kAngleBits));
        // The return's planning update (177) aims him at his podium: he rests on it by 189,
        // where the turn left is 0, through the hand-back on 232 and the shrink after it.
        ticks(value, 146);
        CHECK(value.sumnerYaw() == towardCircle);
        ticks(value, 1);
        CHECK(value.sumnerYaw() == towardCircle + .2f);
        ticks(value, 12);
        const f32 settled = value.sumnerYaw();
        CHECK(settled == Catch::Approx(rest).margin(kAngleBits));
        for (s32 tick = 190; tick < 2000 && value.presenting(); ++tick) {
            CAPTURE(tick);
            value.update(kTick);
            CHECK(value.sumnerYaw() == settled);
        }
        CHECK_FALSE(value.presenting());
    }
    // Where each door's and the shard's shot leaves him as its script returns: Castle's turn
    // wraps the short way past pi, so his yaw runs on beyond it.
    const auto settled = [&](const LegendsProgress& after) {
        auto value = ceremony();
        value.begin({}, after);
        value.start({}, {}, party);
        for (s32 tick = 0; tick < 2000 && value.current() != nullptr; ++tick) {
            value.update(kTick);
        }
        const f32 yaw = value.sumnerYaw();
        for (s32 tick = 0; tick < 2000 && value.presenting(); ++tick) {
            value.update(kTick);
        }
        // The return brings him round to rest: the Ice door's a whole turn below it.
        CHECK(std::remainder(value.sumnerYaw() - rest, kTwoPi) ==
              Catch::Approx(0).margin(kAngleBits * 10));
        return yaw;
    };
    // Doors in LegendsTowerProgress order: Town, Castle, Ice and Battlefield.
    constexpr std::array<f32, 4> kDoors{.958643854f, 4.29416466f, -.803577960f, .112727612f};
    for (usize i = 0; i < kDoors.size(); ++i) {
        CAPTURE(i);
        LegendsProgress opened;
        opened.obelisks = LegendsTowerProgress::kDoorObelisks[i];
        CHECK(settled(opened) == Catch::Approx(kDoors[i]).margin(kAngleBits * 10));
    }
    LegendsProgress shard;
    shard.shards = 1;
    CHECK(settled(shard) == Catch::Approx(2.97644401f).margin(kAngleBits * 10));
}

TEST_CASE("N64 ceremony turns the held party to Tower.bin's foci and back at its hand-back",
          "[legends][tower-ceremony][assets]") {
    // Two players at tower2's entrance, held from the visit's opening with the headings they
    // came in at; the facings are Tower.bin's turn in float32 worked apart from this code.
    auto value = ceremony();
    LegendsProgress now;
    now.runestones = 1;
    value.begin({}, now);
    const std::array<Vec3, 2> feet{Vec3{-6, 0, -16}, Vec3{-3, 0, -15}};
    constexpr std::array<f32, 2> kArrival{0, .5f};
    for (usize seat = 0; seat < feet.size(); ++seat) {
        CHECK_FALSE(value.hold(seat, feet[seat], kArrival[seat]));
    }
    value.start({}, {}, (feet[0] + feet[1]) * .5f);
    constexpr f32 kAngleBits = 1e-6f;
    constexpr std::array<f32, 2> kTowardSumner{.982793748f, .909753144f};
    constexpr std::array<f32, 2> kTowardCircle{.244978666f, .153944671f};
    for (s32 tick = 0; tick <= 232; ++tick) {
        CAPTURE(tick);
        if (tick > 0) {
            value.update(kTick);
        }
        for (usize seat = 0; seat < feet.size(); ++seat) {
            CAPTURE(seat);
            // The heading a later hold passes is not kept: the opening's first hold gave it.
            const auto yaw = value.hold(seat, feet[seat], kTowardSumner[seat]);
            REQUIRE(yaw);
            if (tick == 232) {
                // The hand-back's update turns each back to the heading it came in at.
                CHECK(*yaw == kArrival[seat]);
            } else {
                CHECK(*yaw == Catch::Approx(tick < 21 ? kTowardSumner[seat] : kTowardCircle[seat])
                                  .margin(kAngleBits));
            }
        }
    }
    for (usize seat = 0; seat < feet.size(); ++seat) {
        CHECK_FALSE(value.hold(seat, feet[seat], kArrival[seat]));
    }
}

TEST_CASE("N64 Sumner's staff shrinks away over the second after the hand-back",
          "[legends][tower-ceremony]") {
    // Tower.bin e086107c..e08611dc in float32 on each update's dt, the N64's two VIs
    // (0x3d088889), from e0861260's state 1 and zeroed clock: z is what is left of the second,
    // x and y that over .3 once under it. The summed clock leaves z .33333319 on the 20th update
    // after the hand-back, thins the staff from the 21st and reaches 1 on the 30th.
    {
        auto value = nativeCeremony();
        LegendsProgress now;
        now.runestones = 1;
        value.begin({}, now);
        value.start({}, {}, {});
        ticks(value, 232 + 20);
        CHECK(value.staffScale() == Vec3{1, 1, std::bit_cast<f32>(0x3eaaaaa6U)});
        ticks(value, 1);
        CHECK(value.staffScale() == Vec3{std::bit_cast<f32>(0x3f7ffff6U),
                                         std::bit_cast<f32>(0x3f7ffff6U),
                                         std::bit_cast<f32>(0x3e999994U)});
        ticks(value, 8);
        CHECK(value.staffVisible());
        CHECK(value.staffScale() == Vec3{std::bit_cast<f32>(0x3de38d8aU),
                                         std::bit_cast<f32>(0x3de38d8aU),
                                         std::bit_cast<f32>(0x3d088820U)});
        ticks(value, 1);
        CHECK_FALSE(value.staffVisible());
        CHECK_FALSE(value.presenting());
    }
    // 30 and 60 updates a second feed the same updates.
    for (const s32 steps : {1, 2}) {
        CAPTURE(steps);
        auto value = nativeCeremony();
        CHECK_FALSE(value.staffVisible());
        LegendsProgress now;
        now.runestones = 1;
        value.begin({}, now);
        value.start({}, {}, {});
        const auto step = [&] {
            for (s32 i = 0; i < steps; ++i) {
                value.update(kTick / static_cast<f32>(steps));
            }
        };
        for (s32 tick = 1; tick < 232; ++tick) {
            CAPTURE(tick);
            step();
            CHECK(value.staffVisible());
            CHECK(value.staffScale() == Vec3{1});
        }
        CHECK_FALSE(value.takeLookDown());
        // The hand-back skips e0860d1c: the staff keeps its size there.
        step();
        CHECK_FALSE(value.active());
        CHECK(value.presenting());
        CHECK(value.staffVisible());
        CHECK(value.staffScale() == Vec3{1});
        CHECK_FALSE(value.takeLookDown());
        f32 seconds = 0;
        for (s32 update = 1; update <= 30; ++update) {
            CAPTURE(update);
            if (steps == 2) {
                value.update(kTick / 2);
                CHECK_FALSE(value.takeLookDown());
                CHECK(value.staffScale().z == 1 - seconds);
                value.update(kTick / 2);
            } else {
                value.update(kTick);
            }
            // State 0x11's first update looks down; LKDN waits for the idle to take it once.
            CHECK(value.takeLookDown() == (update == 1));
            CHECK_FALSE(value.takeLookDown());
            seconds = std::min(seconds + kTick, 1.0f);
            const f32 length = 1 - seconds;
            const f32 girth = length < .3f ? length / .3f : 1;
            CHECK(value.staffScale() == Vec3{girth, girth, length});
            CHECK(value.staffVisible() == (update < 30));
            CHECK(value.presenting() == (update < 30));
        }
        // Nothing presents after it: further updates change nothing.
        ticks(value, 10);
        CHECK_FALSE(value.staffVisible());
        CHECK_FALSE(value.presenting());
        CHECK_FALSE(value.takeLookDown());
    }
}
