#include <algorithm>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/net/PacketTransport.h"

#include "game/netplay/CharacterProfile.h"

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
    p.lifetime = {10000, 200, 123456, 36000.125};
    return save;
}
TEST_CASE("network character profiles preserve selected progress without exporting local saves",
          "[netplay][character-profile]") {
    const auto source = character();
    const auto profile = CharacterProfile::capture(source);
    REQUIRE(profile);
    const auto packet = CharacterProfilePacket::encode(*profile);
    REQUIRE(packet);
    CHECK(packet->size() == CharacterProfilePacket::kBytes);
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
        std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    std::min(usize{4}, bytes.size() - offset), 0xff);
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
    CHECK_FALSE(CharacterProfile::capture(save));
}
} // namespace
