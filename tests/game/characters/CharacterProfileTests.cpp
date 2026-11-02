#include <algorithm>
#include <array>
#include <format>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/net/PacketTransport.h"

#include "TestSupport.h"
#include "game/characters/CharacterProfile.h"
#include "game/rollback/PartyCheckpoint.h"

namespace {
using namespace gdl;
using namespace gdl::game;
CharacterSave character() {
    CharacterSave save;
    save.name = "KNIGHT";
    save.character = 3;
    save.color = 2;
    save.gold = 99999;
    save.levelTotal = 155;
    save.autoAttack = false;
    save.helpSeen = {150, 0, 31, 32, 70, 150};
    save.moviesSeen = {"local/private/history"};
    save.classUnlock = 511;
    save.classes[0].experience = 1234;
    auto& p = save.progress();
    p.experience = levelExperience(99);
    p.gold = 0; // Stale banked wallet must not supersede the live selected wallet.
    p.promotedLevel = 80;
    p.health = 9999;
    p.fightAdd = 99.5f;
    p.armorAdd = 19.25f;
    p.magicAdd = -2.5f;
    p.speedAdd = 0.5f;
    p.crystals[0] = -20;
    p.crystals[13] = 150;
    p.unlocked = 0x3fff;
    p.inventory.keys = 9;
    p.inventory.potions = {1, 2, 3, 4, 1, 2, 3, 4, 4};
    for (usize i = 0; i < p.inventory.powerups.size(); ++i) {
        p.inventory.powerups[i] = {i % 2 == 0 ? -1.0f : 20.5f, 5 + static_cast<s32>(i % 5), 2.25f,
                                   1U << static_cast<u32>(i), i % 3 != 0};
    }
    p.relics = {8191, 65535, 65535, 4096, 32768, 31, {12, 20, 28}};
    p.levels.beaten.fill(255);
    p.levels.runeLevels = {8191, 127};
    p.levels.legendLevels = {65535, 32768};
    p.levels.bossDeaths = {123, 64};
    p.lifetime = {10000, 200, 123456, 36000.125, 57};
    p.legendsProgress(LegendsPlatform::N64) = {
        0x8000000000000001ULL, 2, 4, 8, {"castle6", "mount1", "town4"}};
    p.legendsProgress(LegendsPlatform::Dreamcast) = {
        16, 32, 64, 0xfedcba9876543210ULL, {"levela1", "levels6"}};
    return save;
}
TEST_CASE("network character profiles preserve selected progress without exporting local saves",
          "[netplay][character-profile]") {
    const auto source = character();
    const auto profile = CharacterProfile::capture(source);
    REQUIRE(profile);
    const auto packet = CharacterProfilePacket::encode(*profile);
    REQUIRE(packet);
    CHECK(packet->size() > CharacterProfilePacket::kMinBytes);
    CHECK(packet->size() <= CharacterProfilePacket::kMaxBytes);
    CHECK(packet->size() + 8 < PacketTransport::kMaxPacketBytes);
    const auto decoded = CharacterProfilePacket::decode(*packet);
    REQUIRE(decoded);
    CHECK(CharacterProfilePacket::encode(*decoded) == packet);
    auto expected = source;
    expected.moviesSeen.clear();
    expected.classUnlock = 0;
    expected.classes[0] = {};
    expected.helpSeen = {0, 31, 32, 70, 150};
    CHECK(decoded->gameplayCopy().toJson() == expected.toJson());
    CHECK(source.classes[0].experience == 1234);
    CHECK(source.moviesSeen.size() == 1);
    CHECK(source.helpSeen.size() == 6);
}
TEST_CASE("network character profiles keep their byte layout", "[netplay][character-profile]") {
    const auto profile = CharacterProfile::capture(character());
    REQUIRE(profile);
    const auto bytes = CharacterProfilePacket::encode(*profile);
    REQUIRE(bytes);
    constexpr std::string_view kProfile =
        "47444350"                         // magic
        "02000000"                         // version
        "4b4e494748540000"                 // name
        "03000000020000009f8601009b000000" // class, colour, gold, levels
        "184205009f860100500000000f270000" // experience, gold, promotion, health
        "00000000"                         // auto attack
        "0000c74200009a41000020c00000003f" // fight, armour, magic, speed
        "ecffffff000000000000000000000000000000000000000000000000" // crystals
        "00000000000000000000000000000000000000000000000096000000"
        "ff3f0000"                 // unlocked
        "09000000"                 // keys
        "09000000"                 // potions held
        "010000000200000003000000" // potions
        "040000000100000002000000"
        "030000000400000004000000"
        "000080bf05000000000010400100000000000000" // powerup slots
        "0000a44106000000000010400200000001000000"
        "000080bf07000000000010400400000001000000"
        "0000a44108000000000010400800000000000000"
        "000080bf09000000000010401000000001000000"
        "0000a44105000000000010402000000001000000"
        "000080bf06000000000010404000000000000000"
        "0000a44107000000000010408000000001000000"
        "000080bf08000000000010400001000001000000"
        "0000a44109000000000010400002000000000000"
        "000080bf05000000000010400004000001000000"
        "ff1f0000ffff0000ffff0000"                 // runes, legends, shards
        "00100000008000001f000000"                 // pending runes, shards, ceremonies
        "0c000000140000001c000000"                 // gargoyle pieces
        "ffffffffffffffffffffffffffff0000"         // levels beaten, padding
        "ff1f00007f000000"                         // rune levels
        "ffff000000800000"                         // legend levels
        "7b00000040000000"                         // boss deaths
        "10270000c800000040e2010039000000"         // kills, generators, gold, streak
        "000000000494e140"                         // play seconds
        "0100008001000000400000000000000000004000" // help seen
        "0100000000000080"                         // N64 runestones
        "0200000000000000"                         // shards
        "0400000000000000"                         // obelisks
        "0800000000000000"                         // legend items
        "03000000"                                 // maps completed
        "0007636173746c6536"                       // "castle6"
        "00066d6f756e7431"                         // "mount1"
        "0005746f776e34"                           // "town4"
        "1000000000000000"                         // Dreamcast runestones
        "2000000000000000"                         // shards
        "4000000000000000"                         // obelisks
        "1032547698badcfe"                         // legend items
        "02000000"                                 // maps completed
        "00076c6576656c6131"                       // "levela1"
        "05027336";                                // "levels6" after "level"
    CHECK(test::hexBytes(*bytes) == kProfile);
}
TEST_CASE("character profile decoding is exact bounded and rejects malformed values",
          "[netplay][character-profile]") {
    const auto original = CharacterProfilePacket::encode(*CharacterProfile::capture(character()));
    REQUIRE(original);
    for (usize length = 0; length < original->size(); ++length) {
        CHECK_FALSE(CharacterProfilePacket::decode(std::span(*original).first(length)));
    }
    auto bytes = *original;
    bytes.push_back(0);
    CHECK_FALSE(CharacterProfilePacket::decode(bytes));
    for (const usize offset : {0U, 4U, 14U, 16U, 20U, 48U, 52U, 128U, 132U, 176U, 188U, 392U, 416U,
                               442U, 444U, 468U, 507U}) {
        bytes = *original;
        // A word of -2: out of every field's bounds, where -1 is a gargoyle record's
        // "congratulated" sentinel and so allowed there.
        std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    std::min(usize{4}, bytes.size() - offset), 0xff);
        bytes[offset] = 0xfe;
        CHECK_FALSE(CharacterProfilePacket::decode(bytes));
    }
    bytes = *original;
    bytes[9] = 0; // Data after a name terminator is not silently ignored.
    CHECK_FALSE(CharacterProfilePacket::decode(bytes));
}
TEST_CASE("local character selection validates indices floats counters and item quantities",
          "[netplay][character-profile]") {
    auto save = character();
    SECTION("negative class") {
        save.character = -1;
    }
    SECTION("past final class") {
        save.character = kClassCount;
    }
    SECTION("invalid name") {
        save.name = "bad\n";
    }
    SECTION("empty name") {
        save.name.clear();
    }
    SECTION("too many keys") {
        save.progress().inventory.keys = 10;
    }
    SECTION("too many potions") {
        save.progress().inventory.potions.push_back(1);
    }
    SECTION("invalid potion") {
        save.progress().inventory.potions[0] = 0;
    }
    SECTION("invalid help") {
        save.helpSeen.push_back(151);
    }
    SECTION("invalid held item") {
        save.progress().inventory.powerups[0].kind = 0;
    }
    SECTION("nonfinite stat") {
        save.progress().magicAdd = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("nonfinite time") {
        save.progress().lifetime.playSeconds = std::numeric_limits<f64>::infinity();
    }
    SECTION("negative wallet") {
        save.gold = -1;
    }
    SECTION("unknown rune") {
        save.progress().relics.runes = 0x8000;
    }
    SECTION("unowned pending shard") {
        save.progress().relics.shards = 0;
    }
    SECTION("unknown ceremony") {
        save.progress().relics.pendingCeremonies = 32;
    }
    SECTION("noncanonical Legends map") {
        save.progress().legends[0].completedMaps = {"Town4"};
    }
    SECTION("duplicate Legends map") {
        save.progress().legends[1].completedMaps = {"levela1", "levela1"};
    }
    CHECK_FALSE(CharacterProfile::capture(save));
}

TEST_CASE("full native Legends completion rosters fit a reliable travel profile losslessly",
          "[netplay][character-profile][legends]") {
    // The 34 N64 and 41 Dreamcast nonempty native worlds, including secret/hub maps.
    constexpr std::array kN64{"battle1", "battle2", "battle3", "boss1",   "boss2",  "castle1",
                              "castle3", "castle4", "castle5", "castle6", "hell1",  "hell2",
                              "ice1",    "ice2",    "ice3",    "ice4",    "ice5",   "mount1",
                              "mount2",  "mount3",  "mount4",  "mount5",  "mount6", "secret2",
                              "secret3", "secret4", "secret5", "secret6", "tower2", "town1",
                              "town2",   "town3",   "town4",   "town5"};
    constexpr std::array kDreamcast{
        "levela1", "levela2", "levela3", "levela4", "levela5", "levela6", "levelb1",
        "levelb2", "levelb3", "levelb4", "levelb5", "levelb6", "levelc1", "levelc2",
        "levelc3", "levelc4", "levelc5", "leveld1", "leveld2", "leveld3", "leveld4",
        "leveld5", "levele1", "levele2", "levelf1", "levelf2", "levelg1", "levelg4",
        "levelh1", "levelh2", "levelh3", "levelh4", "levelh5", "levelh6", "levels1",
        "levels2", "levels3", "levels4", "levels5", "levels6", "levelt1"};
    static_assert(kN64.size() == 34 && kDreamcast.size() == 41);
    auto profile = *CharacterProfile::capture(character());
    profile.progress.legends[0].completedMaps.clear();
    profile.progress.legends[1].completedMaps.clear();
    for (const char* const map : kN64) {
        REQUIRE(profile.progress.legends[0].recordCompleted(map));
    }
    for (const char* const map : kDreamcast) {
        REQUIRE(profile.progress.legends[1].recordCompleted(map));
    }
    const auto packet = CharacterProfilePacket::encode(profile);
    REQUIRE(packet);
    CHECK(packet->size() <= CharacterProfilePacket::kMaxBytes);
    const auto decoded = CharacterProfilePacket::decode(*packet);
    REQUIRE(decoded);
    CHECK(decoded->progress.legends == profile.progress.legends);
    CHECK(CharacterProfilePacket::encode(*decoded) == packet);
    const auto travel = PartyCheckpointPacket::encode({1, 0, profile});
    REQUIRE(travel);
    CHECK(travel->size() <= PacketTransport::kMaxPacketBytes);
    const auto arrived = PartyCheckpointPacket::decode(*travel);
    REQUIRE(arrived);
    CHECK(arrived->profile.progress.legends == profile.progress.legends);
}

TEST_CASE("Legends profile compression rejects malformed and noncanonical map lists",
          "[netplay][character-profile][legends]") {
    auto profile = *CharacterProfile::capture(character());
    profile.progress.legends = {};
    profile.progress.legends[0].completedMaps = {"town1", "town2"};
    const auto packet = CharacterProfilePacket::encode(profile);
    REQUIRE(packet);
    constexpr usize kCount = CharacterProfilePacket::kBaseBytes + 32;
    constexpr usize kFirst = kCount + 4;
    constexpr usize kSecond = kFirst + 7;
    REQUIRE((*packet)[kCount] == 2);
    REQUIRE((*packet)[kFirst] == 0);
    REQUIRE((*packet)[kFirst + 1] == 5);
    REQUIRE((*packet)[kSecond] == 4);
    REQUIRE((*packet)[kSecond + 1] == 1);
    auto bad = *packet;
    SECTION("old protocol version") {
        bad[4] = 1;
    }
    SECTION("oversized completion count") {
        bad[kCount] = 129;
    }
    SECTION("first name cannot refer to a previous prefix") {
        bad[kFirst] = 1;
    }
    SECTION("suffix cannot be empty") {
        bad[kSecond + 1] = 0;
    }
    SECTION("suffix cannot exceed the map name bound") {
        bad[kSecond + 1] = 33;
    }
    SECTION("names must be lowercase canonical asset identities") {
        bad[kFirst + 2] = 'T';
    }
    SECTION("path syntax is rejected") {
        bad[kFirst + 2] = '/';
    }
    SECTION("duplicate map cannot bypass canonical prefix compression") {
        bad[kSecond + 2] = '1';
    }
    SECTION("maps must be sorted") {
        bad[kSecond + 2] = '0';
    }
    SECTION("alternate equivalent prefix encodings are rejected") {
        bad[kSecond] = 3;
        bad[kSecond + 1] = 2;
        bad.insert(bad.begin() + static_cast<std::ptrdiff_t>(kSecond + 2), 'n');
    }
    SECTION("oversized aggregate packet") {
        bad.resize(CharacterProfilePacket::kMaxBytes + 1);
    }
    CHECK_FALSE(CharacterProfilePacket::decode(bad));
}

TEST_CASE("oversized custom Legends histories fail transport without losing saved progress",
          "[netplay][character-profile][legends]") {
    auto save = character();
    save.progress().legends[0].completedMaps.clear();
    for (usize i = 0; i < LegendsProgress::kMaxCompletedMaps; ++i) {
        REQUIRE(save.progress().legends[0].recordCompleted(
            std::format("{:04}{}", i, std::string(28, 'x'))));
    }
    const auto preserved = save.toJson();
    const auto profile = CharacterProfile::capture(save);
    REQUIRE(profile);
    REQUIRE(profile->valid());
    CHECK_FALSE(CharacterProfilePacket::encode(*profile));
    CHECK_FALSE(PartyCheckpointPacket::encode({1, 0, *profile}));
    CHECK(save.toJson() == preserved);
    CHECK(CharacterSave::fromJson(preserved).progress().legends == save.progress().legends);
}
} // namespace
