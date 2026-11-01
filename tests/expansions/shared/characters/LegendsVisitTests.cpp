#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "expansions/shared/characters/LegendsVisit.h"
#include "expansions/shared/level/LegendsItems.h"
#include "game/characters/AutoSave.h"
#include "game/party/PartyRecords.h"

namespace {
using namespace gdl;
using namespace gdl::game;
} // namespace

TEST_CASE("A Legends visit returns earned goods without moving GDL progression",
          "[legends][save]") {
    CharacterSave before;
    before.name = "TEST";
    before.gold = 20;
    auto working = before;
    LegendsPickup coin;
    coin.offer = {static_cast<s32>(ItemKind::Gold), 100};
    REQUIRE(takeLegendsPickup(coin, LegendsPlatform::N64, working).took());
    LegendsPickup rune;
    rune.award = LegendsAward{LegendsRelic::Runestone, 0};
    REQUIRE(takeLegendsPickup(rune, LegendsPlatform::N64, working).took());
    auto live = before;
    REQUIRE(commitLegendsVisit(live, before, working));
    CHECK(live.gold == 120);
    CHECK(live.progress().legendsProgress(LegendsPlatform::N64).runestones == 1);
    CHECK(live.progress().legendsProgress(LegendsPlatform::Dreamcast).runestones == 0);
    CHECK(live.progress().relics == before.progress().relics);
    CHECK(live.progress().levels == before.progress().levels);
    const auto loaded = CharacterSave::fromJson(live.toJson());
    CHECK(loaded.gold == live.gold);
    CHECK(loaded.progress().legends == live.progress().legends);
    CHECK(loaded.progress().relics == live.progress().relics);
}

TEST_CASE("A Legends visit cannot overwrite concurrent console edits or a different character",
          "[legends][save]") {
    CharacterSave before;
    before.name = "FIRST";
    auto working = before;
    working.gold = 100;
    auto live = before;
    SECTION("the parked character received a console grant") {
        live.gold = 1;
    }
    SECTION("another class was changed") {
        live.classes[1].experience = 200;
    }
    SECTION("the working character was replaced") {
        working.name = "OTHER";
    }
    SECTION("the working class was replaced") {
        working.character = 1;
    }
    const auto preserved = live;
    CHECK_FALSE(commitLegendsVisit(live, before, working));
    CHECK(live == preserved);
}

TEST_CASE("Abandoning native level earnings retains only its permanent campaign awards",
          "[legends][save][native-progress-flow]") {
    CharacterSave entry;
    entry.name = "TEST";
    entry.gold = 20;
    entry.progress().experience = 100;
    entry.progress().legendsProgress(LegendsPlatform::Dreamcast).runestones = 4;
    // A real visit starts from a canonical save: the active class wallet and
    // promoted appearance are synchronized by the save boundary.
    entry = CharacterSave::fromJson(entry.toJson());
    auto current = entry;
    current.gold = 500;
    current.progress().experience = 1000;
    auto& campaign = current.progress().legendsProgress(LegendsPlatform::N64);
    campaign.runestones = 3;
    campaign.obelisks = 7;
    campaign.shards = 1;
    REQUIRE(campaign.recordCompleted("mount6"));
    current.progress().relics.runes = 12;
    current.progress().legendsProgress(LegendsPlatform::Dreamcast).runestones = 8;
    auto restored = entry;
    REQUIRE(retainLegendsProgress(restored, current, LegendsPlatform::N64));
    CHECK(restored.gold == entry.gold);
    CHECK(restored.experience() == entry.experience());
    CHECK(restored.progress().relics == entry.progress().relics);
    CHECK(restored.progress().legendsProgress(LegendsPlatform::Dreamcast) ==
          entry.progress().legendsProgress(LegendsPlatform::Dreamcast));
    CHECK(restored.progress().legendsProgress(LegendsPlatform::N64) == campaign);
    const auto loaded = CharacterSave::fromJson(restored.toJson());
    CHECK(loaded == restored);
    auto wrong = current;
    wrong.character = 1;
    const auto before = restored;
    CHECK_FALSE(retainLegendsProgress(restored, wrong, LegendsPlatform::N64));
    CHECK_FALSE(retainLegendsProgress(restored, current, static_cast<LegendsPlatform>(3)));
    CHECK(restored == before);
}

TEST_CASE("Departed native characters keep permanent awards through failed-visit autosave",
          "[legends][save][native-progress-flow]") {
    std::array<PlayerRuntime, 2> runtimes;
    std::vector<PartyMember> participants;
    for (usize i = 0; i < runtimes.size(); ++i) {
        auto& runtime = runtimes[i];
        CharacterSave entry;
        entry.name = i == 0 ? "FIRST" : "SECOND";
        entry.character = static_cast<s32>(i);
        entry.gold = 25;
        entry.progress().experience = 50;
        entry.progress().legendsProgress(LegendsPlatform::Dreamcast).obelisks = 3;
        entry = CharacterSave::fromJson(entry.toJson());
        runtime.actor.spawn(static_cast<s32>(i), entry, nullptr, {}, 0);
        runtime.entrySave = entry;
        runtime.slot = i + 1;
        runtime.life = PlayerLife::Dying;
        runtime.departed = true;
        auto& current = runtime.actor.save();
        current.gold = 500;
        current.progress().experience = 5000;
        current.progress().health = 0;
        auto& progress = current.progress().legendsProgress(LegendsPlatform::N64);
        progress.runestones = 5;
        progress.obelisks = 0x87;
        progress.shards = 3;
        REQUIRE(progress.recordCompleted("boss2"));
        participants.push_back({static_cast<s32>(i), current, runtime.slot});
    }
    // This is why GameOver must snapshot actual runtimes before teardown.
    CHECK(PartyRecords::members(runtimes).empty());
    auto restored = PartyRecords::abandoned(runtimes, participants);
    for (usize i = 0; i < restored.size(); ++i) {
        REQUIRE(retainLegendsProgress(restored[i].save, runtimes[i].actor.save(),
                                      LegendsPlatform::N64));
        CHECK_FALSE(restored[i].fallen);
        CHECK(restored[i].save.gold == 25);
        CHECK(restored[i].save.experience() == 50);
        CHECK(restored[i].slot == i + 1);
        CHECK_FALSE(restored[i]
                        .save.progress()
                        .legendsProgress(LegendsPlatform::N64)
                        .hasCompleted("mount1"));
    }
    SaveSlots unavailable;
    CHECK(autoSaveParty(unavailable, restored).unavailable == 2);
    // A failed write must leave the candidate available for retry, not consume
    // the rewards or mutate an unrelated class/platform.
    SaveSlots slots;
    REQUIRE(slots.open(test::scratchDirectory("legends-departed-progress"), 4));
    CHECK(autoSaveParty(slots, restored).written == 2);
    for (usize i = 0; i < restored.size(); ++i) {
        CharacterSave loaded;
        REQUIRE(slots.load(i + 1, loaded));
        CHECK(loaded == restored[i].save);
        CHECK(loaded.progress().legendsProgress(LegendsPlatform::N64) ==
              runtimes[i].actor.save().progress().legendsProgress(LegendsPlatform::N64));
        CHECK(loaded.progress().relics == runtimes[i].entrySave.progress().relics);
        CHECK(loaded.progress().legendsProgress(LegendsPlatform::Dreamcast).obelisks == 3);
        CHECK(loaded.classes[2] == runtimes[i].entrySave.classes[2]);
    }
}
