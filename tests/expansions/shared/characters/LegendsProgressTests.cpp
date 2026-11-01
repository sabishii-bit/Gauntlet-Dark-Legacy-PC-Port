#include <format>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/io/NativeStateDigest.h"

#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "expansions/shared/characters/LegendsProgress.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Json = nlohmann::json;

NativeStateDigest::Digest digestOf(const CharacterSave& save) {
    NativeStateDigest digest;
    digest.value(save);
    const auto result = digest.checkedFinish();
    REQUIRE(result);
    return *result;
}

TEST_CASE("Legends collectible identities are bounded independent bit sets",
          "[legends][legends-progress]") {
    for (const auto kind : {LegendsRelic::Runestone, LegendsRelic::Shard, LegendsRelic::Obelisk,
                            LegendsRelic::LegendItem}) {
        LegendsProgress progress;
        CHECK_FALSE(progress.has(kind, 0));
        for (s32 index = 0; index < LegendsProgress::kMaxRelics; ++index) {
            CHECK(progress.add(kind, index));
            CHECK(progress.has(kind, index));
            CHECK_FALSE(progress.add(kind, index));
        }
        CHECK(progress.valid());
        const auto full = progress;
        for (const s32 invalid : {-1, LegendsProgress::kMaxRelics, 1000}) {
            CHECK_FALSE(progress.add(kind, invalid));
            CHECK_FALSE(progress.has(kind, invalid));
        }
        CHECK_FALSE(progress.add(static_cast<LegendsRelic>(255), 0));
        CHECK_FALSE(progress.has(static_cast<LegendsRelic>(255), 0));
        CHECK(progress == full);
        CHECK(progress.runestones == (kind == LegendsRelic::Runestone ? ~u64{0} : 0));
        CHECK(progress.shards == (kind == LegendsRelic::Shard ? ~u64{0} : 0));
        CHECK(progress.obelisks == (kind == LegendsRelic::Obelisk ? ~u64{0} : 0));
        CHECK(progress.legendItems == (kind == LegendsRelic::LegendItem ? ~u64{0} : 0));
    }
}

TEST_CASE("Legends map completion is canonical bounded and never accepts paths",
          "[legends][legends-progress]") {
    LegendsProgress progress;
    REQUIRE(progress.recordCompleted("TOWN4"));
    REQUIRE(progress.recordCompleted("mount1"));
    CHECK(progress.completedMaps == std::vector<std::string>{"mount1", "town4"});
    CHECK(progress.hasCompleted("Town4"));
    CHECK_FALSE(progress.recordCompleted("tOwN4"));
    CHECK_FALSE(progress.hasCompleted("town3"));
    for (const std::string invalid : {"", "../town4", "levels/town4", "levels\\town4", "town4.rom",
                                      "bad name", "bad:map", "\x80"}) {
        CHECK_FALSE(progress.recordCompleted(invalid));
        CHECK_FALSE(progress.hasCompleted(invalid));
        CHECK_FALSE(LegendsProgress::mapIdentity(invalid));
    }
    CHECK_FALSE(progress.recordCompleted(std::string(LegendsProgress::kMaxMapName + 1, 'a')));
    progress = {};
    for (usize map = 0; map < LegendsProgress::kMaxCompletedMaps; ++map) {
        CHECK(progress.recordCompleted(std::format("map_{:03}", map)));
    }
    CHECK(progress.valid());
    CHECK_FALSE(progress.recordCompleted("another"));
    const auto before = progress;
    CHECK_FALSE(progress.recordCompleted("map_000"));
    CHECK(progress == before);
    progress.completedMaps[0] = "UPPERCASE";
    CHECK_FALSE(progress.valid());
    CHECK_FALSE(progress.recordCompleted("another"));
}

TEST_CASE("Legends collections are separate from GDL and from other platforms and classes",
          "[legends][legends-progress][save]") {
    CharacterSave save;
    save.name = "STUDY";
    save.progress().relics.runes = 0x123;
    save.progress().relics.shards = 0x42;
    save.progress().levels.recordBeaten(1, 2);
    const auto relics = save.progress().relics;
    const auto levels = save.progress().levels;
    auto& n64 = save.progress().legendsProgress(LegendsPlatform::N64);
    REQUIRE(n64.add(LegendsRelic::Runestone, 12));
    REQUIRE(n64.add(LegendsRelic::Shard, 63));
    REQUIRE(n64.recordCompleted("mount1"));
    auto& dreamcast = save.progress().legendsProgress(LegendsPlatform::Dreamcast);
    REQUIRE(dreamcast.add(LegendsRelic::Obelisk, 3));
    REQUIRE(dreamcast.add(LegendsRelic::LegendItem, 2));
    REQUIRE(dreamcast.recordCompleted("levela1"));
    CHECK_FALSE(dreamcast.has(LegendsRelic::Runestone, 12));
    CHECK_FALSE(n64.has(LegendsRelic::Obelisk, 3));
    CHECK(save.progress().relics == relics);
    CHECK(save.progress().levels == levels);
    const auto collection = save.progress().legends;
    save.selectClass(1);
    CHECK(save.progress().legends == decltype(collection){});
    REQUIRE(save.progress().legendsProgress(LegendsPlatform::N64).add(LegendsRelic::Runestone, 0));
    save.selectClass(0);
    CHECK(save.progress().legends == collection);
    const auto loaded = CharacterSave::fromJson(save.toJson());
    CHECK(loaded.progress().legends == collection);
    CHECK(loaded.classes[1].legends == save.classes[1].legends);
    CHECK(loaded.progress().relics == relics);
    CHECK(loaded.progress().levels == levels);
    CHECK(loaded.toJson() == save.toJson());
}

TEST_CASE("Legacy character saves start empty Legends campaigns without copying GDL awards",
          "[legends][legends-progress][save]") {
    const auto old = CharacterSave::fromJson(
        R"({"name":"OLD","character":0,"classes":{"WAR":{"relics":{"runes":8191,"shards":65535,"legends":65535},"levels":{"beaten":[255]}}}})");
    for (const auto& progress : old.classes) {
        CHECK(progress.legends == decltype(progress.legends){});
    }
    CHECK(old.progress().relics.runes == 8191);
    CHECK(old.progress().relics.shards == 65535);
    CHECK(old.progress().levels.beaten[0] == 255);
    const auto partial = CharacterSave::fromJson(
        R"({"name":"PART","character":0,"classes":{"WAR":{"legends":{"n64":{"obelisks":4}}}}})");
    CHECK(partial.progress().legendsProgress(LegendsPlatform::N64).obelisks == 4);
    CHECK(partial.progress().legendsProgress(LegendsPlatform::Dreamcast) == LegendsProgress{});
}

TEST_CASE("Legends save fields reject malformed identities and masks without truncation",
          "[legends][legends-progress][save]") {
    CharacterSave source;
    source.name = "CHECK";
    const auto baseline = Json::parse(source.toJson());
    for (const Json& invalid : {Json(-1), Json(1.5), Json(true), Json("18446744073709551616"),
                                Json::array(), Json(nullptr)}) {
        for (const char* const field : {"runestones", "shards", "obelisks", "legendItems"}) {
            auto document = baseline;
            document["classes"]["WAR"]["legends"]["n64"][field] = invalid;
            CHECK_THROWS_AS(CharacterSave::fromJson(document.dump()), FormatError);
        }
    }
    auto document = baseline;
    document["classes"]["WAR"]["legends"]["n64"]["runestones"] = std::numeric_limits<u64>::max();
    CHECK(CharacterSave::fromJson(document.dump())
              .progress()
              .legendsProgress(LegendsPlatform::N64)
              .runestones == std::numeric_limits<u64>::max());
    for (const Json& invalid :
         {Json::array({"a", "a"}), Json::array({"b", "a"}), Json::array({"TOWN4"}),
          Json::array({"../town4"}), Json::array({""}), Json::array({false}), Json("town4"),
          Json::array({std::string(LegendsProgress::kMaxMapName + 1, 'a')})}) {
        document = baseline;
        document["classes"]["WAR"]["legends"]["dreamcast"]["completedMaps"] = invalid;
        CHECK_THROWS_AS(CharacterSave::fromJson(document.dump()), FormatError);
    }
    document = baseline;
    document["classes"]["WAR"]["legends"]["n64"]["completedMaps"] =
        std::vector<std::string>(LegendsProgress::kMaxCompletedMaps + 1, "a");
    CHECK_THROWS_AS(CharacterSave::fromJson(document.dump()), FormatError);
    document = baseline;
    document["classes"]["WAR"]["legends"] = Json::array();
    CHECK_THROWS_AS(CharacterSave::fromJson(document.dump()), FormatError);
    document = baseline;
    document["classes"]["WAR"]["legends"]["n64"] = nullptr;
    CHECK_THROWS_AS(CharacterSave::fromJson(document.dump()), FormatError);
}

TEST_CASE("Every Legends campaign field participates in full native character digests",
          "[legends][legends-progress][native-state-digest]") {
    const CharacterSave base;
    const auto before = digestOf(base);
    for (const usize character : {usize{0}, usize{1}, usize{kClassCount - 1}}) {
        for (const auto platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
            for (const auto kind : {LegendsRelic::Runestone, LegendsRelic::Shard,
                                    LegendsRelic::Obelisk, LegendsRelic::LegendItem}) {
                auto changed = base;
                REQUIRE(changed.classes[character].legendsProgress(platform).add(kind, 63));
                CHECK(digestOf(changed) != before);
                CHECK(digestOf(base) == before);
            }
            auto changed = base;
            REQUIRE(changed.classes[character].legendsProgress(platform).recordCompleted("map1"));
            CHECK(digestOf(changed) != before);
            const auto first = digestOf(changed);
            REQUIRE(changed.classes[character].legendsProgress(platform).recordCompleted("map2"));
            CHECK(digestOf(changed) != first);
            changed = base;
            CHECK(digestOf(changed) == before);
        }
    }
}

TEST_CASE("Saving Legends progress reuses the character slot and refuses invalid replacement",
          "[legends][legends-progress][save]") {
    SaveSlots slots;
    REQUIRE(slots.open(test::scratchDirectory("legends-progress-save"), 2));
    CharacterSave save;
    save.name = "SAVED";
    REQUIRE(save.progress().legendsProgress(LegendsPlatform::N64).add(LegendsRelic::Runestone, 0));
    REQUIRE(slots.write(0, save));
    REQUIRE(save.progress().legendsProgress(LegendsPlatform::Dreamcast).recordCompleted("levela1"));
    REQUIRE(slots.write(0, save));
    CharacterSave loaded;
    REQUIRE(slots.load(0, loaded));
    CHECK(loaded.progress().legends == save.progress().legends);
    CHECK_FALSE(slots.slot(1).occupied);
    save.progress().legendsProgress(LegendsPlatform::N64).completedMaps = {"../invalid"};
    CHECK_FALSE(slots.write(0, save));
    CharacterSave preserved;
    REQUIRE(slots.load(0, preserved));
    CHECK(preserved == loaded);
}
} // namespace
