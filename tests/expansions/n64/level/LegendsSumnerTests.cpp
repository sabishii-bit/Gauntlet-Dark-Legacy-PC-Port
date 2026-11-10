#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/level/LegendsSumner.h"
#include "expansions/shared/level/LegendsItems.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    return test::dataDirectory().parent_path() / "assets/n64/extracted/_program";
}

bool installed() {
    return std::filesystem::is_regular_file(nativeRoot() / "game.bin") &&
           std::filesystem::is_regular_file(nativeRoot() / "Tower.bin");
}

LegendsSumner native() {
    return LegendsSumner::decode(readFile(nativeRoot() / "game.bin"),
                                 readFile(nativeRoot() / "Tower.bin"));
}
} // namespace

TEST_CASE("Native departure reminders require genuinely missing runes or obelisks",
          "[legends][sumner][native-progress-flow]") {
    std::array<LegendsProgress, 2> party{};
    LegendsPickup rune;
    rune.award = LegendsAward{LegendsRelic::Runestone, 0};
    LegendsPickup obelisk;
    obelisk.award = LegendsAward{LegendsRelic::Obelisk, 0};
    std::array pickups{rune, obelisk};
    CHECK(legendsN64MissedRelicCue(pickups, party) == 0x4057);
    REQUIRE(party[1].add(LegendsRelic::Obelisk, 0));
    for (s32 index = 0; index < 13; ++index) {
        pickups[0].award->index = index;
        CHECK(legendsN64MissedRelicCue(pickups, party) == 0x4059U + index);
        REQUIRE(party[1].add(LegendsRelic::Runestone, index));
        CHECK_FALSE(legendsN64MissedRelicCue(pickups, party));
    }
    party = {};
    pickups[0].collected = true;
    pickups[1].collected = true;
    CHECK_FALSE(legendsN64MissedRelicCue(pickups, party));
    pickups[0].collected = false;
    pickups[0].award = LegendsAward{LegendsRelic::Shard, 0};
    CHECK_FALSE(legendsN64MissedRelicCue(pickups, party));
    pickups[0].award = LegendsAward{LegendsRelic::Runestone, 13};
    CHECK_FALSE(legendsN64MissedRelicCue(pickups, party));
    CHECK_FALSE(legendsN64MissedRelicCue(pickups, {}));
    pickups[0].award->index = 0;
    party[0].completedMaps = {"../invalid"};
    CHECK_FALSE(legendsN64MissedRelicCue(pickups, party));
}

TEST_CASE("N64 Sumner requires authenticated optional programs", "[legends][sumner]") {
    LegendsSumner empty;
    CHECK_FALSE(empty.ready());
    CHECK(empty.choices().empty());
    const std::array<LegendsProgress, 1> party{};
    CHECK_FALSE(empty.adviceTextId(0xd, party));
    CHECK_FALSE(empty.historyPage(14, party));
    CHECK_FALSE(empty.podiumContact({0, 0, 0}, {1, 0, 0}, 1, 4));
    CHECK_FALSE(empty.update(2, std::array{LegendsSumnerVisitor{0, true, true}}));
    CHECK_THROWS_AS(LegendsSumner::decode({}, {}), FormatError);
    if (installed()) {
        auto program = readFile(nativeRoot() / "game.bin");
        const auto tower = readFile(nativeRoot() / "Tower.bin");
        CHECK_THROWS_AS(LegendsSumner::decode(program, {}), FormatError);
        program.back() ^= 1;
        CHECK_THROWS_AS(LegendsSumner::decode(program, tower), FormatError);
    }
}

TEST_CASE("Native stage victory departure phrases override only their authored stages",
          "[legends][sumner][native-progress-flow]") {
    std::array<LegendsProgress, 1> party{};
    LegendsPickup rune;
    rune.award = LegendsAward{LegendsRelic::Runestone, 0};
    const std::array pickups{rune};
    constexpr std::array<std::pair<std::string_view, u32>, 7> kStages{{{"mount6", 0x4067},
                                                                       {"castle5", 0x4068},
                                                                       {"town5", 0x4069},
                                                                       {"ice5", 0x406a},
                                                                       {"boss2", 0x406b},
                                                                       {"boss1", 0x406c},
                                                                       {"hell1", 0x406d}}};
    for (const auto& [stage, token] : kStages) {
        CAPTURE(stage);
        CHECK(legendsN64DepartureCue(stage, pickups, party) == token);
        CHECK(legendsN64DepartureCue(stage, {}, party) == token);
    }
    CHECK(legendsN64DepartureCue("BOSS2", {}, party) == 0x406b);
    CHECK(legendsN64DepartureCue("mount1", pickups, party) == 0x4059);
    CHECK_FALSE(legendsN64DepartureCue("hell2", {}, party));
    CHECK_FALSE(legendsN64DepartureCue("tower2", {}, party));
    party[0].runestones = 0x1fff;
    party[0].obelisks = 0xff;
    CHECK_FALSE(legendsN64DepartureCue("mount1", pickups, party));
    CHECK(legendsN64DepartureCue("boss2", pickups, party) == 0x406b);
    CHECK_FALSE(legendsN64DepartureCue("boss2", {}, {}));
}

TEST_CASE("N64 Sumner keeps the authored menu and exact missing relic order",
          "[legends][sumner][assets]") {
    if (!installed()) {
        SKIP("Native N64 programs are not installed");
    }
    const auto sumner = native();
    REQUIRE(sumner.ready());
    CHECK(sumner.titleId() == 441);
    REQUIRE(sumner.choices().size() == 6);
    constexpr std::array<u32, 6> kTexts{426, 430, 442, 443, 444, 445};
    constexpr std::array<u32, 6> kActions{0x11, 0x15, 0x2e, 0xd, 0x2f, 2};
    for (usize i = 0; i < kTexts.size(); ++i) {
        CHECK(sumner.choices()[i].textId == kTexts[i]);
        CHECK(sumner.choices()[i].action == kActions[i]);
    }
    std::array<LegendsProgress, 4> party{};
    constexpr std::array<s32, 7> kObelisks{1, 2, 3, 6, 7, 4, 5};
    constexpr std::array<u32, 7> kWorldReplies{492, 493, 494, 497, 498, 495, 496};
    for (usize i = 0; i < kObelisks.size(); ++i) {
        CAPTURE(i);
        CHECK(sumner.adviceTextId(0xd, party) == kWorldReplies[i]);
        REQUIRE(party[i % party.size()].add(LegendsRelic::Obelisk, kObelisks[i] - 1));
    }
    CHECK(sumner.adviceTextId(0xd, party) == 520);
    constexpr std::array<s32, 13> kRunes{4, 5, 6, 1, 2, 3, 10, 11, 12, 7, 8, 9, 13};
    constexpr std::array<u32, 13> kRuneReplies{500, 501, 502, 506, 507, 508, 503,
                                               504, 505, 509, 510, 511, 512};
    for (usize i = 0; i < kRunes.size(); ++i) {
        CAPTURE(i);
        CHECK(sumner.adviceTextId(0x2e, party) == kRuneReplies[i]);
        REQUIRE(party[i % party.size()].add(LegendsRelic::Runestone, kRunes[i] - 1));
    }
    CHECK(sumner.adviceTextId(0x2e, party) == 519);
    CHECK_FALSE(sumner.adviceTextId(0x11, party));
    CHECK_FALSE(sumner.adviceTextId(0x15, party));
    CHECK_FALSE(sumner.adviceTextId(2, party));
    CHECK_FALSE(sumner.adviceTextId(0xffffffffU, party));
    CHECK_FALSE(sumner.adviceTextId(0xd, {}));
    party[0].completedMaps = {"../invalid"};
    CHECK_FALSE(sumner.adviceTextId(0xd, party));
}

TEST_CASE("N64 enemy hints require native boss completions and the Battlefield obelisk",
          "[legends][sumner][assets]") {
    if (!installed()) {
        SKIP("Native N64 programs are not installed");
    }
    const auto sumner = native();
    std::array<LegendsProgress, 4> party{};
    // Owning shards/legend weapons is not proof that the native boss was beaten.
    party[0].shards = ~u64{0};
    party[1].legendItems = ~u64{0};
    CHECK(sumner.adviceTextId(0x2f, party) == 513);
    REQUIRE(party[0].recordCompleted("mount6"));
    CHECK(sumner.adviceTextId(0x2f, party) == 515);
    REQUIRE(party[1].recordCompleted("castle5"));
    CHECK(sumner.adviceTextId(0x2f, party) == 514);
    REQUIRE(party[2].recordCompleted("town5"));
    CHECK(sumner.adviceTextId(0x2f, party) == 516);
    REQUIRE(party[3].recordCompleted("ice5"));
    CHECK(sumner.adviceTextId(0x2f, party) == 499);
    REQUIRE(party[0].recordCompleted("boss2"));
    CHECK(sumner.adviceTextId(0x2f, party) == 499); // Actual obelisk8 award is required.
    REQUIRE(party[1].add(LegendsRelic::Obelisk, 7));
    CHECK(sumner.adviceTextId(0x2f, party) == 517);
    REQUIRE(party[2].recordCompleted("hell2"));
    CHECK(sumner.adviceTextId(0x2f, party) == 518);
}

TEST_CASE("N64 History retains the displayed milestone rather than guessing from relics",
          "[legends][sumner][assets]") {
    if (!installed()) {
        SKIP("Native N64 programs are not installed");
    }
    const auto sumner = native();
    std::array<LegendsProgress, 1> party{};
    constexpr std::array<std::pair<u32, u8>, 11> kPages{{
        {0, 12},
        {2, 10},
        {3, 10},
        {5, 16},
        {6, 13},
        {7, 11},
        {10, 17},
        {12, 15},
        {13, 14},
        {14, 1},
        {15, 2},
    }};
    for (const auto& [state, page] : kPages) {
        CAPTURE(state);
        CHECK(sumner.historyPage(state, party) == page);
    }
    party[0].runestones = ~u64{0};
    party[0].obelisks = ~u64{0};
    CHECK(sumner.historyPage(14, party) == 1);
    REQUIRE(party[0].recordCompleted("mount1"));
    CHECK_FALSE(sumner.historyPage(3, party));
    CHECK(sumner.historyPage(2, party) == 10);
    CHECK_FALSE(sumner.historyPage(16, party));
    CHECK_FALSE(sumner.historyPage(0xffffffffU, party));
}

TEST_CASE("Native Sumner restores advice chapters from campaign progress",
          "[legends][sumner][native-progress-flow][assets]") {
    if (!installed()) {
        SKIP("Native N64 programs are not installed");
    }
    const auto sumner = native();
    std::array<LegendsProgress, 2> party{};
    CHECK(sumner.milestone(party) == 14);
    REQUIRE(party[0].recordCompleted("mount1"));
    CHECK(sumner.milestone(party) == 2);
    party[0].obelisks = 3;
    party[1].obelisks = 4;
    CHECK(sumner.milestone(party) == 0);
    party[0].obelisks |= 0x60;
    CHECK(sumner.milestone(party) == 7);
    party[1].obelisks |= 0x18;
    CHECK(sumner.milestone(party) == 6);
    party[0].shards = 1;
    party[1].shards = 14;
    CHECK(sumner.milestone(party) == 13);
    party[0].obelisks |= 0x80;
    CHECK(sumner.milestone(party) == 12);
    party[0].runestones = 0x1fff;
    CHECK(sumner.milestone(party) == 5);
    REQUIRE(party[1].recordCompleted("hell2"));
    CHECK(sumner.milestone(party) == 10);
}

TEST_CASE("N64 Sumner contact reuses the corrected podium movement and native cooldown",
          "[legends][sumner][assets]") {
    if (!installed()) {
        SKIP("Native N64 programs are not installed");
    }
    auto sumner = native();
    const auto tower = formats::decodeLegendsN64Tower(readFile(nativeRoot() / "Tower.bin"));
    const auto& podium = tower.residentVolumes[1];
    const auto world = [&](Vec3 local) {
        const f32 c = std::cos(podium.yaw);
        const f32 s = std::sin(podium.yaw);
        return podium.centre + Vec3{c * local.x + s * local.z, local.y, -s * local.x + c * local.z};
    };
    CHECK(sumner.podiumContact(world({2, -2.5f, 0}), world({0, -2.5f, 0}), .5f, 3));
    CHECK_FALSE(sumner.podiumContact(world({2, 6, 0}), world({0, 6, 0}), .5f, 3));
    CHECK_FALSE(sumner.podiumContact(world({2, -8, 0}), world({0, -8, 0}), .5f, 3));
    CHECK_FALSE(sumner.podiumContact(world({2, 0, 0}), world({3, 0, 0}), .5f, 3));
    CHECK_FALSE(sumner.podiumContact(world({2, 0, 0}), world({2, 0, 0}), .5f, 3));
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(sumner.podiumContact({nan, 0, 0}, world({0, 0, 0}), .5f, 3));
    CHECK_FALSE(sumner.podiumContact(world({2, 0, 0}), world({0, 0, 0}), -1, 3));

    const std::array visitors{
        LegendsSumnerVisitor{3, true, true},  LegendsSumnerVisitor{0, true, false},
        LegendsSumnerVisitor{2, true, true},  LegendsSumnerVisitor{1, false, true},
        LegendsSumnerVisitor{-1, true, true}, LegendsSumnerVisitor{4, true, true}};
    CHECK_FALSE(sumner.update(1, visitors));        // Native comparison is strictly greater.
    CHECK_FALSE(sumner.update(10, visitors, true)); // Modal/pause time cannot run the clock.
    CHECK_FALSE(sumner.update(0, visitors));
    CHECK_FALSE(sumner.update(nan, visitors));
    CHECK_FALSE(sumner.update(-1, visitors));
    CHECK(sumner.update(.01f, visitors) == 2);
    CHECK_FALSE(sumner.update(0, visitors));
    CHECK_FALSE(sumner.update(1, visitors));
    CHECK_FALSE(sumner.update(.01f, std::array{LegendsSumnerVisitor{0, true, false}}));
    CHECK(sumner.update(0, std::array{LegendsSumnerVisitor{0, true, true}}) == 0);
}
