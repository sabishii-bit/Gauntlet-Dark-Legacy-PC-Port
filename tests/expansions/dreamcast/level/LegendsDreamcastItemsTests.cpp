#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/shared/level/LegendsItems.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"

namespace {
using namespace gdl;
using namespace gdl::formats;
using namespace gdl::game;

struct Fixture {
    ItemInfoRecord info;
    ItemInstanceRecord instance;
    Fixture() {
        info.type = 1;
        info.subtype = 1;
        info.collisionType = 1;
        info.radius = 0.5f;
        info.height = 1.0f;
        info.name = "COIN_GOLD";
        info.activeType = 0x10;
        info.value = 500;
        instance.position = Vec3{3, 4, 5};
    }
};
} // namespace

TEST_CASE("Dreamcast loose goods retain their native amounts and placement identities",
          "[legends][pickups][dreamcast]") {
    Fixture fixture;
    std::string diagnostic = "old warning";
    auto pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 37, -1, &diagnostic);
    REQUIRE(pickup);
    CHECK(diagnostic.empty());
    CHECK(pickup->nativeIndex == 37);
    CHECK(pickup->position == fixture.instance.position);
    CHECK(pickup->radius == 0.5f);
    CHECK(pickup->height == 1.0f);
    CHECK(pickup->name == "COIN_GOLD");
    CHECK(pickup->offer.kind == static_cast<s32>(ItemKind::Gold));
    CHECK(pickup->offer.amount == 500);
    CHECK_FALSE(pickup->award);

    fixture.info.subtype = 2;
    fixture.info.value = 3;
    fixture.instance.params[0] = 9;
    fixture.instance.name = "KEYRING#01";
    pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 38);
    REQUIRE(pickup);
    CHECK(pickup->offer.kind == static_cast<s32>(ItemKind::Keys));
    CHECK(pickup->offer.amount == 3); // DC does not override loose keys from params.
    CHECK(pickup->name == "KEYRING#01");

    fixture.info.subtype = 3;
    fixture.info.value = -50;
    pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 39);
    REQUIRE(pickup);
    CHECK(pickup->offer.kind == static_cast<s32>(ItemKind::Food));
    CHECK(pickup->offer.amount == -50);

    fixture.info.subtype = 4;
    fixture.info.value = 1;
    for (u32 colour = 1; colour <= 4; ++colour) {
        fixture.info.properties = colour;
        pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 40);
        REQUIRE(pickup);
        CHECK(pickup->offer.kind == static_cast<s32>(ItemKind::Potion));
        CHECK(pickup->offer.flags == colour);
    }
    fixture.info.properties = 5;
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 40));
    fixture.info.properties = 0;
    std::string reason;
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 40, -1, &reason));
    CHECK_FALSE(reason.empty()); // Never invent a player colour for an unresolved bottle.
}

TEST_CASE("Dreamcast native powerup properties map only to verified inventory effects",
          "[legends][pickups][dreamcast]") {
    Fixture fixture;
    const std::array<std::pair<s32, u32>, 33> known{{
        {5, 1},
        {5, 2},
        {5, 3},
        {5, 4},
        {5, damage::kThreeWayShot},
        {5, damage::kFiveWayShot},
        {5, damage::kSuperShot},
        {5, damage::kReflect},
        {5, damage::kHammer},
        {5, damage::kRapidFire},
        {6, armor::kInvulnerable},
        {6, armor::kInvulnerable | armor::kGoldInvulnerable},
        {6, armor::kReflect},
        {6, 0x80000},
        {6, armor::kFireArmor},
        {6, armor::kElectricArmor},
        {7, 0},
        {8, 0},
        {9, powerup::kLevitation},
        {9, powerup::kXRay},
        {9, powerup::kInvisible},
        {9, powerup::kStopTime},
        {9, powerup::kFireBreath},
        {9, powerup::kAcidBreath},
        {9, powerup::kLightningBreath},
        {9, powerup::kPhoenix},
        {9, powerup::kGrowth},
        {9, powerup::kEnemyShrink},
        {9, powerup::kPojo},
        {9, powerup::kSkorneHorns},
        {9, powerup::kSkorneMask},
        {9, powerup::kRightGauntlet},
        {9, powerup::kLeftGauntlet},
    }};
    fixture.info.value = 0;
    fixture.info.activeOn = 30;
    for (const auto& [kind, flags] : known) {
        CAPTURE(kind, flags);
        fixture.info.subtype = kind;
        fixture.info.properties = flags;
        const auto pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 2);
        REQUIRE(pickup);
        CHECK(pickup->offer.kind == kind);
        CHECK(pickup->offer.flags == flags);
        CHECK(pickup->offer.strength == 30);
        CHECK_FALSE(pickup->award);
    }
    fixture.info.subtype = 5;
    fixture.info.properties = damage::kHammer;
    fixture.info.value = 3;
    fixture.info.activeOn = -1;
    auto pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 2);
    REQUIRE(pickup);
    CHECK(pickup->offer.amount == 3);
    CHECK(pickup->offer.strength == -1);

    fixture.info.subtype = 9;
    fixture.info.properties = powerup::kTurbo; // GDL-only property, not DC Turbo.
    std::string reason;
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 2, -1, &reason));
    CHECK_FALSE(reason.empty());
    fixture.info.properties = powerup::kPojo | powerup::kLevitation;
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 2));
}

TEST_CASE("Dreamcast chicken behavior follows the native definition rather than artwork aliases",
          "[legends][pickups][dreamcast]") {
    Fixture fixture;
    fixture.info.subtype = 3;
    fixture.info.name = "CHICKEN";
    fixture.info.value = 100;
    fixture.instance.name = "CHICKEN#02";
    auto pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 7);
    REQUIRE(pickup);
    CHECK(pickup->harmsPojo);
    CHECK(pickup->offer.amount == 100);
    CHECK(pickup->name == "CHICKEN#02");

    fixture.info.name = "HAM";
    fixture.instance.name = "CHICKEN";
    pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 7);
    REQUIRE(pickup);
    CHECK_FALSE(pickup->harmsPojo);
    fixture.info.subtype = 1;
    fixture.info.name = "CHICKEN";
    pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 7);
    REQUIRE(pickup);
    CHECK_FALSE(pickup->harmsPojo);
}

TEST_CASE("Dreamcast runes obelisks and quest items use separate native campaign identities",
          "[legends][pickups][dreamcast]") {
    Fixture fixture;
    fixture.info.subtype = 10;
    for (s16 rune = 0; rune < 13; ++rune) {
        fixture.info.value = rune;
        const auto pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 90);
        REQUIRE(pickup);
        REQUIRE(pickup->award);
        CHECK(pickup->award->kind == LegendsRelic::Runestone);
        CHECK(pickup->award->index == rune);
        CHECK(pickup->offer.kind == 0);
        CHECK_FALSE(pickup->staysVisible);
    }
    fixture.info.value = 13;
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 90));
    fixture.info.subtype = 13;
    fixture.info.value = 0;
    constexpr std::array<s32, 9> kExpected{0, 2, 1, 4, 3, 5, 6, 0, 0};
    for (s32 realm = 0; realm < static_cast<s32>(kExpected.size()); ++realm) {
        const auto pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 91, realm);
        REQUIRE(pickup);
        REQUIRE(pickup->award);
        CHECK(pickup->award->kind == LegendsRelic::LegendItem);
        CHECK(pickup->award->index == kExpected[static_cast<usize>(realm)]);
        CHECK(pickup->offer.kind == 0);
    }
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 91));
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 91, 9));

    fixture.info.subtype = 12;
    fixture.info.activeType = 0x16;
    fixture.info.collisionType = 3;
    fixture.info.radius = 3.9f;
    fixture.info.height = 8.0f;
    fixture.info.xSize = fixture.info.zSize = 1.9f;
    fixture.instance.rotation = Vec3{0, 0.75f, 0};
    for (u8 obelisk = 0; obelisk < 9; ++obelisk) {
        fixture.instance.params[0] = obelisk;
        const auto pickup = legendsDreamcastPickup(fixture.info, fixture.instance, 92);
        REQUIRE(pickup);
        REQUIRE(pickup->award);
        CHECK(pickup->award->kind == LegendsRelic::Obelisk);
        CHECK(pickup->award->index == obelisk);
        CHECK(pickup->offer.kind == 0);
        CHECK(pickup->staysVisible);
        CHECK(pickup->collision == LegendsPickupCollision::Box);
        CHECK(pickup->radius == Catch::Approx(4.29f));
        CHECK(pickup->xSize == Catch::Approx(2.09f));
        CHECK(pickup->zSize == Catch::Approx(2.09f));
        CHECK(pickup->height == 8.0f);
        CHECK(pickup->yaw == 0.75f);
    }
    fixture.info.xSize = 3;
    fixture.info.zSize = 0.5f;
    const auto longBox = legendsDreamcastPickup(fixture.info, fixture.instance, 92);
    REQUIRE(longBox);
    // 8c0ece60 stores local X as (cos(yaw), 0, -sin(yaw)). The
    // collision query inverse-projects onto those native rendered axes.
    const Vec3 longAxis{2 * std::cos(0.75f), 0, -2 * std::sin(0.75f)};
    const Vec3 shortAxis{-longAxis.z, 0, longAxis.x};
    CHECK(touchesLegendsPickup(*longBox, longBox->position + longAxis, 0.1f, 1));
    CHECK_FALSE(touchesLegendsPickup(*longBox, longBox->position + shortAxis, 0.1f, 1));
    fixture.instance.params[0] = 9;
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 92));
    fixture.instance.params[0] = 0;
    fixture.instance.params[1] = 0xff;
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 92));
    fixture.instance.params[1] = 0;
    fixture.instance.rotation.x = 0.5f;
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 92));
}

TEST_CASE("Dreamcast pickup adapter refuses hidden activated and unsupported placements",
          "[legends][pickups][dreamcast]") {
    Fixture fixture;
    SECTION("container contents are not placed pickups") {
        fixture.info.type = 2;
        fixture.instance.params[0] = 1;
    }
    SECTION("enemy placement") {
        fixture.info.type = 4;
    }
    SECTION("random choice") {
        fixture.info.type = -1;
    }
    SECTION("instance activation") {
        fixture.instance.flags = 1;
    }
    SECTION("invisible instance") {
        fixture.instance.flags = 2;
    }
    SECTION("unknown instance flag") {
        fixture.instance.flags = 0x80;
    }
    SECTION("invisible object") {
        fixture.info.objectFlags = 2;
    }
    SECTION("unknown activation") {
        fixture.info.activeType = 0x50;
    }
    SECTION("disabled collision") {
        fixture.info.collisionType = 0;
    }
    SECTION("unknown shape") {
        fixture.info.collisionType = 2;
    }
    SECTION("unknown collision flag") {
        fixture.info.collisionFlags = 1;
    }
    SECTION("offset shape") {
        fixture.info.collisionOffset.x = 1;
    }
    SECTION("negative radius") {
        fixture.info.radius = -1;
    }
    SECTION("negative height") {
        fixture.info.height = -1;
    }
    SECTION("nonfinite position") {
        fixture.instance.position.y = std::numeric_limits<f32>::infinity();
    }
    SECTION("nonfinite rotation") {
        fixture.instance.rotation.y = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("boss key is not a touch awarded shard") {
        fixture.info.subtype = 11;
    }
    SECTION("unknown subtype") {
        fixture.info.subtype = 15;
    }
    SECTION("negative currency") {
        fixture.info.value = -5;
    }
    std::string reason;
    CHECK_FALSE(legendsDreamcastPickup(fixture.info, fixture.instance, 1, -1, &reason));
    CHECK(reason.empty() == (fixture.info.type != 1));
}

TEST_CASE("Dreamcast pickup adapter covers every native placed pickup",
          "[legends][pickups][dreamcast][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_directory(root / "levels")) {
        SKIP("Dreamcast native files are not extracted");
    }
    usize levels = 0;
    std::array<usize, 14> counts{};
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!entry.is_directory() || !entry.path().filename().string().starts_with("level")) {
            continue;
        }
        CAPTURE(entry.path().string());
        const auto scene = LegendsDreamcastScene::parse(readFile(entry.path() / "worlds.rom"));
        const auto scope = dreamcastLegendsFixtureScope(
            readFile(root / dreamcastLegendsWorldDataFile(entry.path().filename().string())),
            entry.path().filename().string());
        for (usize i = 0; i < scene.itemInstances.size(); ++i) {
            const auto& instance = scene.itemInstances[i];
            const auto& info = scene.itemInfos[static_cast<usize>(instance.info)];
            if (info.type != 1) {
                continue;
            }
            std::string reason;
            const auto pickup = legendsDreamcastPickup(info, instance, static_cast<u32>(i),
                                                       scope ? scope->realmType : -1, &reason);
            CAPTURE(i, info.name, info.subtype, info.properties, reason);
            REQUIRE(pickup);
            CHECK(pickup->nativeIndex == i);
            CHECK(pickup->position == instance.position);
            CHECK(pickup->award.has_value() == (info.subtype >= 10));
            CHECK(pickup->harmsPojo == (info.subtype == 3 && info.name == "CHICKEN"));
            REQUIRE(info.subtype > 0);
            REQUIRE(static_cast<usize>(info.subtype) < counts.size());
            ++counts[static_cast<usize>(info.subtype)];
        }
        ++levels;
    }
    CHECK(levels == 41);
    constexpr std::array<usize, 14> kExpected{0,  713, 358, 164, 105, 62, 19,
                                              11, 0,   46,  13,  0,   9,  5};
    CHECK(counts == kExpected);
}
