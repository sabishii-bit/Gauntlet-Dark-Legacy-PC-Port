#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "expansions/shared/level/LegendsItems.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/PowerupFlags.h"

namespace {
using namespace gdl;
using namespace gdl::game;
} // namespace

TEST_CASE("Legends pickups retain GDL full-wallet and partial-key rules", "[legends][items]") {
    CharacterSave save;
    LegendsPickup pickup;
    pickup.offer = {static_cast<s32>(ItemKind::Gold), 100};
    save.gold = 99999;
    CHECK(takeLegendsPickup(pickup, LegendsPlatform::N64, save).outcome ==
          ItemTaking::Outcome::GoldFull);
    CHECK_FALSE(pickup.collected);
    save.gold = 99989;
    CHECK(takeLegendsPickup(pickup, LegendsPlatform::N64, save).took());
    CHECK(pickup.collected);
    CHECK(save.gold == 99999);
    CHECK_FALSE(takeLegendsPickup(pickup, LegendsPlatform::N64, save).took());

    pickup = {};
    pickup.offer = {static_cast<s32>(ItemKind::Keys), 5};
    save.progress().inventory.keys = 8;
    CHECK(takeLegendsPickup(pickup, LegendsPlatform::Dreamcast, save).outcome ==
          ItemTaking::Outcome::PartTaken);
    CHECK_FALSE(pickup.collected);
    CHECK(pickup.offer.amount == 4);
    CHECK(save.progress().inventory.keys == 9);
    save.progress().inventory.keys = 0;
    CHECK(takeLegendsPickup(pickup, LegendsPlatform::Dreamcast, save).took());
    CHECK(pickup.collected);
    CHECK(save.progress().inventory.keys == 4);
}

TEST_CASE("Legends pickup awards cannot leak into another campaign", "[legends][items]") {
    CharacterSave save;
    const auto gdl = save.progress().relics;
    for (const auto platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        for (const auto kind : {LegendsRelic::Runestone, LegendsRelic::Shard, LegendsRelic::Obelisk,
                                LegendsRelic::LegendItem}) {
            LegendsPickup pickup;
            pickup.award = LegendsAward{kind, 3};
            CHECK(takeLegendsPickup(pickup, platform, save).took());
            CHECK(save.progress().legendsProgress(platform).has(kind, 3));
            CHECK(save.progress().relics == gdl);
            pickup.collected = false;
            CHECK(takeLegendsPickup(pickup, platform, save).outcome ==
                  ItemTaking::Outcome::AlreadyHeld);
            CHECK_FALSE(pickup.collected);
        }
    }
    LegendsPickup native;
    native.offer = {static_cast<s32>(ItemKind::Runestone), 4};
    CHECK_FALSE(takeLegendsPickup(native, LegendsPlatform::N64, save).took());
    CHECK(save.progress().relics == gdl);
    native.award = LegendsAward{LegendsRelic::Runestone, 64};
    CHECK_FALSE(takeLegendsPickup(native, LegendsPlatform::N64, save).took());
    native.award->index = 0;
    CHECK_FALSE(takeLegendsPickup(native, static_cast<LegendsPlatform>(5), save).took());
}

TEST_CASE("Native chicken resolves against the current Pojo form without rewriting the offer",
          "[legends][items][pojo]") {
    CharacterSave save;
    save.progress().health = 400;
    LegendsPickup pickup;
    pickup.offer = {static_cast<s32>(ItemKind::Food), 100};
    pickup.harmsPojo = true;
    bool harmful = false;
    SECTION("ordinary character") {}
    SECTION("temporary Pojo active") {
        save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 60);
        harmful = true;
    }
    SECTION("permanent Pojo") {
        save.progress().inventory.permanentPojo = true;
        harmful = true;
    }
    SECTION("temporary Pojo switched off") {
        save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 60, false);
    }
    SECTION("temporary Pojo expired") {
        save.progress().inventory.addPowerup(powerup::kSpecial, powerup::kPojo, 0, 60);
        save.progress().inventory.advance(60);
    }
    SECTION("other food remains healthy for Pojo") {
        save.progress().inventory.permanentPojo = true;
        pickup.harmsPojo = false;
    }
    const auto taken = takeLegendsPickup(pickup, LegendsPlatform::Dreamcast, save);
    REQUIRE(taken.took());
    CHECK(pickup.collected);
    CHECK(pickup.offer.amount == 100);
    CHECK(save.health() == (harmful ? 350 : 500));
    CHECK(taken.hurt == harmful);
    CHECK(taken.ate == !harmful);
}

TEST_CASE("Native party awards reach selected joined classes even when the collector owns them",
          "[legends][items][legends-progress]") {
    std::array<CharacterSave, 4> saves;
    for (usize i = 0; i < saves.size(); ++i) {
        saves[i].name = "PARTY";
        saves[i].selectClass(static_cast<s32>(i));
    }
    const std::array<CharacterSave*, 4> party{saves.data(), &saves[1], &saves[2], &saves[3]};
    for (const auto kind : {LegendsRelic::Runestone, LegendsRelic::Shard, LegendsRelic::Obelisk}) {
        LegendsPickup pickup;
        pickup.award = LegendsAward{kind, 3};
        pickup.awardShared = true;
        pickup.consumeWhenOwned = true;
        CHECK_FALSE(legendsAwardOwnedByParty(*pickup.award, LegendsPlatform::N64, party));
        REQUIRE(saves[0].progress().legendsProgress(LegendsPlatform::N64).add(kind, 3));
        CHECK(legendsAwardOwnedByParty(*pickup.award, LegendsPlatform::N64, party));
        REQUIRE(takeLegendsPickup(pickup, LegendsPlatform::N64, saves[0], 1, true, party).took());
        CHECK(pickup.collected);
        for (usize i = 0; i < saves.size(); ++i) {
            CHECK(saves[i].progress().legendsProgress(LegendsPlatform::N64).has(kind, 3));
            CHECK_FALSE(
                saves[i].progress().legendsProgress(LegendsPlatform::Dreamcast).has(kind, 3));
            CHECK(saves[i].progress().relics == Relics{});
            CHECK_FALSE(saves[i]
                            .classes[(i + 1) % saves.size()]
                            .legendsProgress(LegendsPlatform::N64)
                            .has(kind, 3));
            const auto loaded = CharacterSave::fromJson(saves[i].toJson());
            CHECK(loaded.progress().legends == saves[i].progress().legends);
        }
        pickup.collected = false;
        CHECK(takeLegendsPickup(pickup, LegendsPlatform::N64, saves[0], 1, true, party).took());
        CHECK(pickup.collected);
    }
}

TEST_CASE("Shared Legends award boundary ignores invalid entries and does not share personal items",
          "[legends][items][legends-progress]") {
    CharacterSave collector;
    CharacterSave friendSave;
    CharacterSave invalid;
    invalid.character = -1;
    const auto originalInvalid = invalid;
    const std::array<CharacterSave*, 4> party{nullptr, &invalid, &friendSave, &friendSave};
    LegendsPickup pickup;
    pickup.award = LegendsAward{LegendsRelic::LegendItem, 2};
    REQUIRE(takeLegendsPickup(pickup, LegendsPlatform::N64, collector, 1, true, party).took());
    CHECK_FALSE(friendSave.progress()
                    .legendsProgress(LegendsPlatform::N64)
                    .has(LegendsRelic::LegendItem, 2));
    CHECK(invalid == originalInvalid);
    pickup = {};
    pickup.award = LegendsAward{LegendsRelic::Shard, 2};
    pickup.awardShared = true;
    REQUIRE(takeLegendsPickup(pickup, LegendsPlatform::N64, collector, 1, true, party).took());
    CHECK(friendSave.progress().legendsProgress(LegendsPlatform::N64).shards == 4);
    CHECK(collector.progress().legendsProgress(LegendsPlatform::N64).shards == 4);
    CHECK(invalid == originalInvalid);
    const auto before = friendSave;
    pickup.collected = false;
    pickup.award->index = 64;
    CHECK_FALSE(takeLegendsPickup(pickup, LegendsPlatform::N64, collector, 1, true, party).took());
    CHECK_FALSE(pickup.collected);
    CHECK(friendSave == before);
    CHECK_FALSE(legendsAwardOwnedByParty(*pickup.award, LegendsPlatform::N64, party));
    pickup.award->index = 2;
    CHECK_FALSE(legendsAwardOwnedByParty(*pickup.award, LegendsPlatform::Dreamcast, party));
    CHECK_FALSE(legendsAwardOwnedByParty(*pickup.award, static_cast<LegendsPlatform>(5), party));
}

TEST_CASE("Pojo chicken uses normal poisoned-food rules even at full health",
          "[legends][items][pojo]") {
    CharacterSave save;
    save.progress().health = 500;
    save.progress().inventory.permanentPojo = true;
    LegendsPickup pickup;
    pickup.offer = {static_cast<s32>(ItemKind::Food), 100};
    pickup.harmsPojo = true;
    s32 expected = 450;
    SECTION("unprotected Pojo can eat chicken at full health") {}
    SECTION("gold invulnerability converts the poison into healing") {
        save.progress().inventory.addPowerup(
            powerup::kArmor, armor::kInvulnerable | armor::kGoldInvulnerable, 0, 30);
        expected = 505;
    }
    CHECK(takeLegendsPickup(pickup, LegendsPlatform::Dreamcast, save).took());
    CHECK(save.health() == expected);
    CHECK(pickup.offer.amount == 100);
}

TEST_CASE("Legends contacts respect height, rotated native boxes and consumed items",
          "[legends][items]") {
    LegendsPickup pickup;
    CHECK(touchesLegendsPickup(pickup, {1.5f, 4, 0}, 1, 1));
    CHECK_FALSE(touchesLegendsPickup(pickup, {1.51f, 0, 0}, 1, 1));
    CHECK_FALSE(touchesLegendsPickup(pickup, {0, 4.01f, 0}, 1, 1));
    pickup.collected = true;
    CHECK_FALSE(touchesLegendsPickup(pickup, {}, 1, 1));
    pickup.collected = false;
    pickup.collision = LegendsPickupCollision::Box;
    pickup.radius = 4;
    pickup.xSize = 3;
    pickup.zSize = 1;
    CHECK(touchesLegendsPickup(pickup, {3, 0, 0}, 0, 1));
    CHECK_FALSE(touchesLegendsPickup(pickup, {0, 0, 3}, 0, 1));
    pickup.yaw = kPi / 2;
    CHECK(touchesLegendsPickup(pickup, {0, 0, 3}, 0, 1));
    CHECK_FALSE(touchesLegendsPickup(pickup, {3, 0, 0}, 0, 1));
    pickup.yaw = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(touchesLegendsPickup(pickup, {}, 1, 1));
}

TEST_CASE("Legends rejects malformed grants without consuming a pickup", "[legends][items]") {
    CharacterSave save;
    const auto before = save;
    LegendsPickup pickup;
    SECTION("negative gold") {
        pickup.offer = {1, -100};
    }
    SECTION("overflowing count") {
        pickup.offer = {1, std::numeric_limits<s32>::max()};
    }
    SECTION("nonfinite effect strength") {
        pickup.offer = {5, 0, 1, std::numeric_limits<f32>::infinity()};
    }
    SECTION("overflowing scaled duration") {
        pickup.offer = {5, 0, 1, 1000000};
    }
    CHECK_FALSE(takeLegendsPickup(pickup, LegendsPlatform::N64, save, 2).took());
    CHECK_FALSE(pickup.collected);
    CHECK(save == before);
}
