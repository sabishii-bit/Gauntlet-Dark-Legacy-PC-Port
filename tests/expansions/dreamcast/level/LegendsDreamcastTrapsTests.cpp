#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/assets/LegendsDreamcastArchive.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/level/LegendsDreamcastTraps.h"
#include "game/combat/DamageTypes.h"

namespace {
using namespace gdl;
using namespace gdl::game;

formats::ItemInfoRecord flame() {
    formats::ItemInfoRecord info;
    info.type = 8;
    info.subtype = 1;
    info.name = "FLAMEH";
    info.collisionType = 3;
    info.radius = 4.25f;
    info.height = 3;
    info.xSize = .6f;
    info.zSize = 3;
    info.properties = 1;
    info.value = 20;
    info.activeType = 5;
    info.activeOff = -40;
    return info;
}

formats::ItemInfoRecord spikes() {
    auto info = flame();
    info.subtype = 0;
    info.name = "SPIKES";
    info.radius = 3.5f;
    info.height = 5;
    info.xSize = 2;
    info.zSize = .4f;
    info.properties = 0x2000;
    return info;
}

std::filesystem::path nativeRoot() {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "levels/levela1/worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    return root;
}

void reachAction(LegendsDreamcastTrap& trap, s32 action) {
    for (s32 tick = 0; tick < 240 && trap.action() != action; ++tick) {
        trap.advance(1.f / 60, false);
    }
    REQUIRE(trap.action() == action);
}

TEST_CASE("Dreamcast spikes hurt only in ON with physical spike damage",
          "[legends][dreamcast-traps][dreamcast-spikes]") {
    auto trap = LegendsDreamcastTrap::decode(spikes(), {}, 23, {0, 4, 6, 4});
    REQUIRE(trap);
    CHECK(trap->ticksLeft() >= 40);
    CHECK(trap->ticksLeft() < 120);
    LegendsPortalVisitor visitor;
    CHECK_FALSE(trap->touch(visitor));
    reachAction(*trap, 1);
    CHECK_FALSE(trap->dangerous());
    CHECK_FALSE(trap->touch(visitor));
    reachAction(*trap, 2);
    REQUIRE(trap->dangerous());
    visitor.levitating = true;
    CHECK_FALSE(trap->touch(visitor));
    visitor.levitating = false;
    for (u8 player = 0; player < 4; ++player) {
        visitor.player = player;
        const auto hit = trap->touch(visitor);
        REQUIRE(hit);
        CHECK(hit->nativeIndex == 23);
        CHECK(hit->player == player);
        CHECK(hit->damage == 20);
        CHECK(hit->pierces);
        // ItemTouch adds stun to the authored 0x2000 physical hit and
        // separately excludes levitation, expressed by the common low flag.
        CHECK(hit->impact.flags == (damage::kSpike | damage::kStun | damage::kLow));
        CHECK((hit->impact.flags & damage::kElementMask) == 0);
        CHECK_FALSE(trap->touch(visitor));
    }
    reachAction(*trap, 3);
    CHECK_FALSE(trap->dangerous());
    CHECK_FALSE(trap->touch(visitor));
    reachAction(*trap, 0);
    CHECK_FALSE(trap->touch(visitor));
    reachAction(*trap, 2);
    REQUIRE(trap->touch(visitor));
    const s32 remaining = trap->ticksLeft();
    trap->advance(1.f / 60, true);
    CHECK(trap->action() == 0);
    CHECK(trap->ticksLeft() == remaining - 1);
    CHECK_FALSE(trap->dangerous());
    CHECK_FALSE(trap->touch(visitor));
    trap->advance(1, true);
    CHECK(trap->action() == 0);
    reachAction(*trap, 2);
    CHECK(trap->touch(visitor));
}

TEST_CASE("Dreamcast Stop Time leaves the trap countdown running without an extra release delay",
          "[legends][dreamcast-traps][dreamcast-spikes]") {
    auto trap = LegendsDreamcastTrap::decode(spikes(), {}, 23, {0, 0, 58, 0});
    REQUIRE(trap);
    reachAction(*trap, 2);
    for (s32 tick = 0; tick < 10; ++tick) {
        trap->advance(1.f / 60, true);
    }
    CHECK(trap->action() == 0);
    CHECK(trap->ticksLeft() == 48);
    CHECK_FALSE(trap->dangerous());
    trap->advance(1.f / 60, false);
    CHECK(trap->action() == 0);
    CHECK(trap->ticksLeft() == 47);
    for (s32 tick = 0; tick < 60; ++tick) {
        trap->advance(1.f / 60, true);
    }
    CHECK(trap->action() == 0);
    CHECK(trap->ticksLeft() == 0);
    trap->advance(1.f / 60, false);
    CHECK(trap->action() == 1);
    trap->advance(1.f / 60, false);
    CHECK(trap->action() == 2);
    CHECK(trap->dangerous());
}

TEST_CASE("Dreamcast spikes accept empty rest and transition sequences but require an attack",
          "[legends][dreamcast-traps][dreamcast-spikes]") {
    auto trap = LegendsDreamcastTrap::decode(spikes(), {}, 23, {0, 0, 58, 0});
    REQUIRE(trap);
    reachAction(*trap, 1);
    CHECK_FALSE(trap->dangerous());
    trap->advance(1.f / 60, false);
    CHECK(trap->action() == 2);
    CHECK(trap->dangerous());
    for (s32 tick = 0; tick < 58; ++tick) {
        trap->advance(1.f / 60, false);
    }
    CHECK(trap->action() == 3);
    CHECK_FALSE(trap->dangerous());
    trap->advance(1.f / 60, false);
    CHECK(trap->action() == 0);
    CHECK_FALSE(LegendsDreamcastTrap::decode(spikes(), {}, 23, {0, 0, 0, 0}));
    CHECK_FALSE(LegendsDreamcastTrap::decode(spikes(), {}, 23, {0, -1, 58, 0}));
}

TEST_CASE("Dreamcast spike contacts respect the authored box and valid player volume",
          "[legends][dreamcast-traps][dreamcast-spikes]") {
    auto info = spikes();
    info.collisionOffset = {0, 0, 4};
    formats::ItemInstanceRecord item;
    item.position = {10, 0, 20};
    item.rotation.y = glm::radians(90.f);
    auto trap = LegendsDreamcastTrap::decode(info, item, 23, {0, 0, 58, 0});
    REQUIRE(trap);
    reachAction(*trap, 2);
    LegendsPortalVisitor visitor;
    visitor.position = {6, 0, 20}; // The old inverse-yaw offset is not the contact centre.
    CHECK_FALSE(trap->touch(visitor));
    visitor.position = {16, 0, 20}; // Native local Z is the narrow side, rotated to world X.
    CHECK_FALSE(trap->touch(visitor));
    visitor.position = {14, 20, 20};
    CHECK_FALSE(trap->touch(visitor));
    visitor.position = {14, 0, 21.5f};
    visitor.alive = false;
    CHECK_FALSE(trap->touch(visitor));
    visitor.alive = true;
    visitor.player = 4;
    CHECK_FALSE(trap->touch(visitor));
    visitor.player = 0;
    visitor.radius = -1;
    CHECK_FALSE(trap->touch(visitor));
    visitor.radius = .5f;
    visitor.height = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(trap->touch(visitor));
    visitor.height = 3;
    CHECK(trap->touch(visitor));
}

TEST_CASE("Dreamcast spike clocks remain identical at thirty and sixty updates per second",
          "[legends][dreamcast-traps][dreamcast-spikes]") {
    auto slow = LegendsDreamcastTrap::decode(spikes(), {}, 23, {0, 10, 16, 64});
    REQUIRE(slow);
    auto fast = *slow;
    for (s32 frame = 0; frame < 360; ++frame) {
        const bool stopped = frame >= 120 && frame < 150;
        slow->advance(1.f / 30, stopped);
        fast.advance(1.f / 60, stopped);
        fast.advance(1.f / 60, stopped);
        CHECK(slow->action() == fast.action());
        CHECK(slow->ticksLeft() == fast.ticksLeft());
        CHECK(slow->generation() == fast.generation());
        CHECK(slow->dangerous() == fast.dangerous());
    }
}

TEST_CASE("Dreamcast contact sounds preserve the subtype and silent native realm entries",
          "[legends][dreamcast-traps][dreamcast-spikes]") {
    const auto spike = LegendsDreamcastTrap::decode(spikes(), {}, 23, {0, 0, 58, 0});
    const auto fire = LegendsDreamcastTrap::decode(flame(), {}, 23, {80, 0, 48, 0});
    REQUIRE(spike);
    REQUIRE(fire);
    // AudioTrap 8c0df0a0 indexes subtype then realm in the table at 8c127698.
    constexpr std::array<u32, 9> kSpikeSounds{0, 0x23000f, 0, 0x240033, 0x25002d, 0, 0, 0, 0};
    constexpr std::array<u32, 9> kFlameSounds{0, 0,        0x210027, 0x240034, 0x250000,
                                              0, 0x2d001f, 0x22001c, 0x26001c};
    for (s32 realm = 0; realm < 9; ++realm) {
        INFO(realm);
        CHECK(spike->contactSound(realm) == kSpikeSounds[static_cast<usize>(realm)]);
        CHECK(fire->contactSound(realm) == kFlameSounds[static_cast<usize>(realm)]);
    }
    for (const s32 realm : {-1, 9, std::numeric_limits<s32>::max()}) {
        CHECK(spike->contactSound(realm) == 0);
        CHECK(fire->contactSound(realm) == 0);
    }
}

TEST_CASE("Dreamcast flame cycles hurt only during native ON and respect Stop Time",
          "[legends][dreamcast-traps]") {
    auto trap = LegendsDreamcastTrap::decode(flame(), {}, 17, {1, 4, 6, 4});
    REQUIRE(trap);
    CHECK(trap->ticksLeft() >= 40);
    CHECK(trap->ticksLeft() < 120);
    LegendsPortalVisitor visitor;
    visitor.position = {0, 0, 2};
    CHECK_FALSE(trap->touch(visitor));
    for (s32 i = 0; i < 121 && trap->action() == 0; ++i) {
        trap->advance(1.f / 60, false);
    }
    REQUIRE(trap->action() == 1);
    CHECK_FALSE(trap->touch(visitor));
    for (s32 i = 0; i < 4; ++i) {
        trap->advance(1.f / 60, false);
    }
    REQUIRE(trap->dangerous());
    auto hit = trap->touch(visitor);
    REQUIRE(hit);
    CHECK(hit->nativeIndex == 17);
    CHECK(hit->damage == 20);
    CHECK((hit->impact.flags & damage::kElementMask) == damage::kFire);
    CHECK_FALSE(trap->touch(visitor));
    visitor.player = 1;
    CHECK(trap->touch(visitor));
    visitor.player = 2;
    visitor.position.y = 20;
    CHECK_FALSE(trap->touch(visitor));
    visitor.position = {10, 0, 2};
    CHECK_FALSE(trap->touch(visitor));
    trap->advance(1.f / 60, true);
    CHECK(trap->action() == 0);
    CHECK_FALSE(trap->dangerous());
    visitor.position = {0, 0, 2};
    CHECK_FALSE(trap->touch(visitor));
}

TEST_CASE("Dreamcast every native spike template binds its realm-specific animation",
          "[legends][dreamcast-traps][dreamcast-spikes][assets]") {
    const auto root = nativeRoot();
    constexpr std::array kRealms{'a', 'c', 'd', 'g', 'h'};
    constexpr std::array<std::array<s32, 4>, 5> kFrames{
        {{0, 0, 29, 0}, {0, 0, 29, 0}, {0, 5, 23, 32}, {0, 0, 28, 0}, {0, 0, 29, 0}}};
    constexpr std::array<std::string_view, 4> kSequenceNames{"OFF", "ONA", "ON", "ONB"};
    std::array<std::array<s32, 4>, kRealms.size()> durations{};
    test::FakeRenderDevice device;
    for (usize realm = 0; realm < kRealms.size(); ++realm) {
        INFO(kRealms[realm]);
        auto archive =
            loadLegendsDreamcastArchive(root / (std::string{"items/level"} + kRealms[realm]));
        REQUIRE(archive);
        const auto index = archive->trees.find("SPIKES");
        REQUIRE(index);
        const auto& tree = archive->trees.tree(*index);
        ItemFigure figure;
        REQUIRE(figure.place(device, *archive, "SPIKES", {}, nullptr));
        for (usize state = 0; state < kSequenceNames.size(); ++state) {
            const auto sequence = tree.findSequence(kSequenceNames[state]);
            REQUIRE(sequence);
            CHECK(tree.sequences[*sequence].frames == kFrames[realm][state]);
            if (kFrames[realm][state] != 0) {
                CHECK(tree.sequences[*sequence].frameRate == (realm == 2 && state == 2 ? 10 : 30));
            }
            durations[realm][state] = figure.ticksOf(static_cast<s32>(*sequence));
        }
    }
    constexpr std::array<std::string_view, 20> kLevels{"a1", "a3", "a4", "a6", "c1", "c2", "c3",
                                                       "c4", "d1", "d2", "d3", "d4", "g1", "g4",
                                                       "h1", "h2", "h3", "h4", "h5", "h6"};
    constexpr std::array<usize, kLevels.size()> kExpected{17, 15, 8, 7, 16, 19, 32, 20, 5,  3,
                                                          3,  7,  5, 6, 23, 18, 16, 15, 17, 26};
    usize total = 0;
    for (usize level = 0; level < kLevels.size(); ++level) {
        INFO(kLevels[level]);
        const auto native = formats::LegendsDreamcastScene::parse(
            readFile(root / ("levels/level" + std::string{kLevels[level]}) / "worlds.rom"));
        const auto realm = static_cast<usize>(std::ranges::find(kRealms, kLevels[level].front()) -
                                              kRealms.begin());
        REQUIRE(realm < kRealms.size());
        const auto& ticks = durations[realm];
        usize count = 0;
        for (usize index = 0; index < native.itemInstances.size(); ++index) {
            const auto& item = native.itemInstances[index];
            const auto& info = native.itemInfos[static_cast<usize>(item.info)];
            if (info.type != 8 || info.subtype != 0) {
                continue;
            }
            INFO(index);
            CHECK(info.name == "SPIKES");
            CHECK(info.properties == 0x2000);
            CHECK(info.value == 20);
            CHECK(info.collisionType == 3);
            CHECK(info.height == 5);
            CHECK(info.xSize == 2);
            CHECK(info.zSize == .4f);
            CHECK(info.activeType == 5);
            CHECK(info.activeOff == -40);
            CHECK(info.activeOn == 0);
            std::string warning;
            REQUIRE(
                LegendsDreamcastTrap::decode(info, item, static_cast<u32>(index), ticks, &warning));
            CHECK(warning.empty());
            ++count;
        }
        CHECK(count == kExpected[level]);
        total += count;
    }
    CHECK(total == 278);
}

TEST_CASE("Dreamcast empty flame windup and recovery remain valid native states",
          "[legends][dreamcast-traps]") {
    auto trap = LegendsDreamcastTrap::decode(flame(), {}, 17, {80, 0, 48, 0});
    REQUIRE(trap);
    for (s32 i = 0; i < 121 && trap->action() == 0; ++i) {
        trap->advance(1.f / 60, false);
    }
    REQUIRE(trap->action() == 1);
    CHECK_FALSE(trap->dangerous());
    trap->advance(1.f / 60, false);
    CHECK(trap->action() == 2);
    CHECK(trap->dangerous());
    trap->advance(48.f / 60, false);
    CHECK(trap->action() == 3);
    CHECK_FALSE(trap->dangerous());
    trap->advance(1.f / 60, false);
    CHECK(trap->action() == 0);
    CHECK_FALSE(LegendsDreamcastTrap::decode(flame(), {}, 17, {80, 0, 0, 0}));
    CHECK_FALSE(LegendsDreamcastTrap::decode(flame(), {}, 17, {80, -1, 48, 0}));
}

TEST_CASE("Dreamcast flame offsets and boxes follow native positive authored yaw",
          "[legends][dreamcast-traps]") {
    auto info = flame();
    info.collisionOffset = {0, 0, 4};
    formats::ItemInstanceRecord item;
    item.position = {10, 0, 20};
    item.rotation.y = glm::radians(90.f);
    auto trap = LegendsDreamcastTrap::decode(info, item, 5, {1, 4, 30, 4});
    REQUIRE(trap);
    for (s32 i = 0; i < 150 && !trap->dangerous(); ++i) {
        trap->advance(1.f / 60, false);
    }
    REQUIRE(trap->dangerous());
    LegendsPortalVisitor visitor;
    visitor.position = {6, 0, 20}; // the old inverse-yaw contact centre
    CHECK_FALSE(trap->touch(visitor));
    visitor.position = {14, 0, 22}; // beyond the narrow side of the turned box
    CHECK_FALSE(trap->touch(visitor));
    visitor.position = {17, 0, 20}; // along native local +Z, now world +X
    CHECK(trap->touch(visitor));
}
} // namespace
