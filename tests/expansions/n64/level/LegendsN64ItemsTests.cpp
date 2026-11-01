#include <array>
#include <filesystem>
#include <limits>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Progress.h"
#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/shared/level/LegendsItems.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/players/PowerupEffects.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using namespace gdl::formats;

LegendsN64Fixture nativePickup(u16 key, s16 amount, u32 flags) {
    LegendsN64Fixture fixture;
    fixture.nativeIndex = 42;
    fixture.position = {1, 2, 3};
    fixture.sequence = "TEST";
    fixture.templateKey = key;
    fixture.resolvedTemplateKey = key;
    fixture.templateFlags = flags;
    fixture.resolvedTemplateFlags = flags;
    fixture.templateAmount = amount;
    return fixture;
}

void checkOffer(const LegendsN64Fixture& fixture, ItemKind kind, s32 amount, u32 flags = 0) {
    const auto pickup = legendsN64Pickup(fixture);
    REQUIRE(pickup);
    CHECK_FALSE(pickup->award);
    CHECK(pickup->nativeIndex == fixture.nativeIndex);
    CHECK(pickup->position == fixture.position);
    CHECK(pickup->radius == 0.5f);
    CHECK(pickup->height == 3);
    CHECK(pickup->offer.kind == static_cast<s32>(kind));
    CHECK(pickup->offer.amount == amount);
    CHECK(pickup->offer.flags == flags);
}
} // namespace

TEST_CASE("N64 pickups translate native quantities and potion elements explicitly",
          "[legends][n64][items]") {
    checkOffer(nativePickup(0, 1, 7), ItemKind::Keys, 1);
    checkOffer(nativePickup(0x0304, 300, 6), ItemKind::Gold, 300);
    checkOffer(nativePickup(0x0401, 50, 1), ItemKind::Food, 50);
    checkOffer(nativePickup(0x0402, -50, 0x10001), ItemKind::Food, -50);
    constexpr std::array<u32, 4> kElements{2, 3, 4, 1};
    for (u16 subtype = 1; subtype <= 4; ++subtype) {
        checkOffer(nativePickup(static_cast<u16>(0x0100U | subtype), 1,
                                0x10008U | (static_cast<u32>(subtype) << 12U)),
                   ItemKind::Potion, 1, kElements[subtype - 1]);
    }
    auto fixture = nativePickup(0, 1, 0x10000007);
    fixture.quantity = 5;
    checkOffer(fixture, ItemKind::Keys, 5);
    fixture.resolvedTemplateFlags &= ~0x10000000U;
    checkOffer(fixture, ItemKind::Keys, 1);
    fixture.resolvedTemplateFlags |= 0x10000000U;
    fixture.quantity = -1;
    CHECK_FALSE(legendsN64Pickup(fixture));
}

TEST_CASE("N64 timed and charged powerups retain their own units under GDL rules",
          "[legends][n64][items]") {
    struct Case {
        u32 native;
        s32 kind;
        u32 flags;
        bool charged;
    };
    const std::array cases{
        Case{0x400840, 5, damage::kReflect, false},
        Case{0x8008b0, 5, damage::kSuperShot, true},
        Case{0x400020, 5, damage::kThreeWayShot, false},
        Case{0x4000e0, 9, powerup::kInvisible, false},
        Case{0x7400c0, 6, armor::kInvulnerable, false},
        Case{0x400860, 5, 1, false},
        Case{0x4008f0, 5, 2, false},
        Case{0x400870, 5, 3, false},
        Case{0x400880, 5, 4, false},
        Case{0x4000d0, 9, powerup::kStopTime, false},
        Case{0x7400a0, 6, 0x80000, false},
        Case{0x400160, 5, damage::kFiveWayShot, false},
        Case{0x740100, 6, armor::kGoldInvulnerable | armor::kInvulnerable, false},
        Case{0x800460, 9, powerup::kFireBreath, true},
        Case{0x8004f0, 9, powerup::kLightningBreath, true},
        Case{0x800480, 9, powerup::kAcidBreath, true},
        Case{0x400110, 9, powerup::kPhoenix, false},
        Case{0x400120, 9, powerup::kGrowth, false},
        Case{0x400130, 9, powerup::kEnemyShrink, false},
        Case{0x400c40, 6, armor::kReflect, false},
        Case{0x400c60, 6, armor::kFireArmor, false},
        Case{0x400cf0, 6, armor::kElectricArmor, false},
        Case{0x800940, 5, damage::kHammer, true},
        Case{0x400050, 5, damage::kRapidFire, false},
        Case{0x400030, 9, powerup::kLevitation, false},
    };
    for (const auto& item : cases) {
        INFO(item.native);
        auto pickup = legendsN64Pickup(nativePickup(0x0201, 20, item.native));
        REQUIRE(pickup);
        CHECK(pickup->offer.kind == item.kind);
        CHECK(pickup->offer.flags == item.flags);
        CHECK(pickup->offer.amount == (item.charged ? 20 : 0));
        CHECK(pickup->offer.strength == (item.charged ? -1 : 20));
        CharacterSave save;
        REQUIRE(takeLegendsPickup(*pickup, LegendsPlatform::N64, save).took());
        auto& inventory = save.progress().inventory;
        REQUIRE(inventory.powerupCount() == 1);
        CHECK(inventory.powerups[0].working());
        inventory.advance(5);
        CHECK(inventory.powerups[0].strength == (item.charged ? -1 : 15));
        if (item.charged) {
            REQUIRE(inventory.spendPowerup(item.kind, item.flags));
            CHECK(inventory.powerups[0].charge == 19);
        }
    }
    CHECK_FALSE(legendsN64Pickup(nativePickup(0x0201, 20, 0xc00840)));
    CHECK_FALSE(legendsN64Pickup(nativePickup(0x0201, 20, 0x840)));
}

TEST_CASE("N64 collectibles award only their isolated native campaign", "[legends][n64][items]") {
    const std::array fixtures{nativePickup(0x150d, 1, 0x8210), nativePickup(0x2b04, 4, 0x8260),
                              nativePickup(0x2904, 1, 0x8201f0)};
    const std::array expected{LegendsAward{LegendsRelic::Runestone, 12},
                              LegendsAward{LegendsRelic::Shard, 3},
                              LegendsAward{LegendsRelic::LegendItem, 3}};
    CharacterSave save;
    const auto original = save.progress().relics;
    for (usize i = 0; i < fixtures.size(); ++i) {
        auto pickup = legendsN64Pickup(fixtures[i]);
        REQUIRE(pickup);
        REQUIRE(pickup->award);
        CHECK(pickup->award->kind == expected[i].kind);
        CHECK(pickup->award->index == expected[i].index);
        CHECK(pickup->awardShared == (i < 2));
        CHECK(pickup->consumeWhenOwned == (i < 2));
        CHECK(pickup->offer.kind == 0);
        REQUIRE(takeLegendsPickup(*pickup, LegendsPlatform::N64, save).took());
        CHECK(save.progress()
                  .legendsProgress(LegendsPlatform::N64)
                  .has(expected[i].kind, expected[i].index));
        CHECK_FALSE(save.progress()
                        .legendsProgress(LegendsPlatform::Dreamcast)
                        .has(expected[i].kind, expected[i].index));
        CHECK(save.progress().relics == original);
    }
    CHECK_FALSE(legendsN64Pickup(nativePickup(0x1510, 1, 0x8210)));
    CHECK_FALSE(legendsN64Pickup(nativePickup(0x2901, 1, 0x8201f0)));
}

TEST_CASE("native Skorne equipment retains charged GDL equipment mechanics",
          "[legends][n64][items][bosses]") {
    constexpr std::array<u32, 4> kIdentities{0x620, 0x630, 0xa40, 0xa50};
    constexpr std::array<u32, 4> kEffects{powerup::kSkorneMask, powerup::kSkorneHorns,
                                          powerup::kRightGauntlet, powerup::kLeftGauntlet};
    for (usize i = 0; i < kIdentities.size(); ++i) {
        CAPTURE(i);
        const s16 amount = i < 2 ? 50 : 100;
        const u16 key = static_cast<u16>(0x2a01U + i);
        auto pickup = legendsN64Pickup(nativePickup(key, amount, 0x840000U | kIdentities[i]));
        REQUIRE(pickup);
        CHECK(pickup->offer.kind == powerup::kSpecial);
        CHECK(pickup->offer.flags == kEffects[i]);
        CHECK(pickup->offer.amount == amount);
        CHECK(pickup->offer.strength == -1);
        CharacterSave save;
        const auto relics = save.progress().relics;
        const f32 armor = save.progress().armorAdd;
        REQUIRE(takeLegendsPickup(*pickup, LegendsPlatform::N64, save).took());
        CHECK(save.progress().relics == relics);
        CHECK(save.progress().armorAdd == armor);
        auto& inventory = save.progress().inventory;
        REQUIRE(inventory.powerupCount() == 1);
        CHECK((PowerupEffects::of(inventory).special & kEffects[i]) != 0);
        inventory.advance(60);
        CHECK(inventory.powerups[0].strength == -1);
        REQUIRE(inventory.spendPowerup(powerup::kSpecial, kEffects[i]));
        CHECK(inventory.powerups[0].charge == amount - 1);
        CHECK_FALSE(
            legendsN64Pickup(nativePickup(key, amount, 0x840000U | kIdentities[(i + 1) % 4])));
        CHECK_FALSE(legendsN64Pickup(nativePickup(key, amount, kIdentities[i])));
    }
}

TEST_CASE("N64 native obelisk contact grants a persistent shared bit and leaves its model present",
          "[legends][n64][items][obelisk]") {
    LegendsN64Obelisk native;
    native.nativeIndex = 71;
    native.identity = 6;
    native.position = {10, 20, 30};
    native.radius = 2;
    native.height = 4;
    native.idleSequence = "OBRD";
    native.spentSequence = "OBDN";
    native.supported = true;
    auto pickup = legendsN64ObeliskPickup(native);
    REQUIRE(pickup);
    CHECK(pickup->nativeIndex == 71);
    CHECK(pickup->position == native.position);
    CHECK(pickup->radius == 2);
    CHECK(pickup->height == 4);
    REQUIRE(pickup->award);
    CHECK(pickup->award->kind == LegendsRelic::Obelisk);
    CHECK(pickup->award->index == 5);
    CHECK(pickup->awardShared);
    CHECK(pickup->consumeWhenOwned);
    CHECK(pickup->staysVisible);
    CHECK(touchesLegendsPickup(*pickup, {12.5f, 20, 30}, .5f, 1));
    CHECK_FALSE(touchesLegendsPickup(*pickup, {12.6f, 20, 30}, .5f, 1));
    CharacterSave collector;
    CharacterSave other;
    const std::array<CharacterSave*, 1> party{&other};
    CHECK(takeLegendsPickup(*pickup, LegendsPlatform::N64, collector, 1, true, party).took());
    CHECK(collector.progress().legendsProgress(LegendsPlatform::N64).obelisks == 0x20);
    CHECK(other.progress().legendsProgress(LegendsPlatform::N64).obelisks == 0x20);
    CHECK_FALSE(touchesLegendsPickup(*pickup, native.position, .5f, 1));
    std::string reason;
    native.minimumPlayers = 2;
    CHECK_FALSE(legendsN64ObeliskPickup(native, &reason));
    CHECK(reason.empty());
    CHECK(legendsN64ObeliskPickup(native, &reason, 2));
    native.minimumPlayers = 0;
    native.supported = false;
    CHECK_FALSE(legendsN64ObeliskPickup(native, &reason));
    CHECK_FALSE(reason.empty());
    native.supported = true;
    native.identity = 0;
    CHECK_FALSE(legendsN64ObeliskPickup(native));
}

TEST_CASE("N64 item adapter refuses inactive malformed and unsupported native consumers",
          "[legends][n64][items]") {
    auto fixture = nativePickup(0, 1, 7);
    std::string reason = "previous";
    fixture.minimumPlayers = 2;
    CHECK_FALSE(legendsN64Pickup(fixture, &reason));
    CHECK(reason.empty());
    CHECK(legendsN64Pickup(fixture, &reason, 2));
    fixture.minimumPlayers = 1;
    fixture.kind = LegendsN64FixtureKind::Container;
    CHECK_FALSE(legendsN64Pickup(fixture, &reason));
    CHECK(reason.empty());
    fixture.kind = LegendsN64FixtureKind::Powerup;
    fixture.sequence.clear();
    CHECK_FALSE(legendsN64Pickup(fixture, &reason));
    CHECK_FALSE(reason.empty());
    fixture.sequence = "KRDY";
    fixture.position.x = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(legendsN64Pickup(fixture, &reason));
    CHECK_FALSE(reason.empty());
    fixture.position = {};
    fixture.pickupHeight = -1;
    CHECK_FALSE(legendsN64Pickup(fixture));
    for (const auto& unsupported :
         {nativePickup(0x0200, 20, 0x400173), nativePickup(0x2101, 1, 0x10000),
          nativePickup(0x281d, 1, 0x190), nativePickup(0x2707, 1, 0x1a4),
          nativePickup(0x2f01, 1, 0x10000280), nativePickup(0x2201, 1, 8)}) {
        CHECK_FALSE(legendsN64Pickup(unsupported, &reason));
        CHECK_FALSE(reason.empty());
    }
}

TEST_CASE("N64 item grants preserve GDL capacity refusals and partial key stacks",
          "[legends][n64][items]") {
    CharacterSave save;
    save.gold = 99999;
    auto gold = legendsN64Pickup(nativePickup(0x0304, 300, 6));
    REQUIRE(gold);
    CHECK(takeLegendsPickup(*gold, LegendsPlatform::N64, save).outcome ==
          ItemTaking::Outcome::GoldFull);
    CHECK_FALSE(gold->collected);
    auto keyFixture = nativePickup(0, 1, 0x10000007);
    keyFixture.quantity = 4;
    auto keys = legendsN64Pickup(keyFixture);
    REQUIRE(keys);
    save.progress().inventory.keys = 7;
    CHECK(takeLegendsPickup(*keys, LegendsPlatform::N64, save).outcome ==
          ItemTaking::Outcome::PartTaken);
    CHECK_FALSE(keys->collected);
    CHECK(keys->offer.amount == 2);
    save.progress().inventory.keys = 0;
    CHECK(takeLegendsPickup(*keys, LegendsPlatform::N64, save).took());
    CHECK(keys->collected);
    CHECK(save.progress().inventory.keys == 2);
}

TEST_CASE("Native N64 map placements retain exact pickup indices and known grant categories",
          "[legends][n64][items][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "items/generic/ppow.rom")) {
        SKIP("Extracted N64 Legends assets unavailable");
    }
    const auto powerups = readFile(root / "items/generic/ppow.rom");
    const auto decorations = readFile(root / "items/generic/pdec.rom");
    const auto generators = readFile(root / "items/generic/pgen.rom");
    const auto traps = readFile(root / "items/generic/ptrap.rom");
    const auto teleports = readFile(root / "items/generic/ptel.rom");
    const auto containers = readFile(root / "items/generic/pcon.rom");
    const auto randoms = readFile(root / "items/generic/prnd.rom");
    const LegendsN64Templates templates{powerups,  decorations, generators, traps,
                                        teleports, containers,  randoms};
    usize maps = 0;
    usize placements = 0;
    std::set<s32> kinds;
    std::set<LegendsRelic> awards;
    std::set<u16> unsupported;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad")) {
            continue;
        }
        INFO(entry.path().filename().string());
        const auto fixtures =
            decodeLegendsN64Fixtures(readFile(entry.path() / "ipr.rom"), templates);
        ++maps;
        for (const auto& fixture : fixtures) {
            if (fixture.kind != LegendsN64FixtureKind::Powerup) {
                continue;
            }
            ++placements;
            std::string reason;
            const auto pickup = legendsN64Pickup(fixture, &reason);
            if (pickup) {
                CHECK(reason.empty());
                CHECK(pickup->nativeIndex == fixture.nativeIndex);
                CHECK(pickup->name == fixture.sequence);
                if (pickup->award) {
                    awards.insert(pickup->award->kind);
                } else {
                    kinds.insert(pickup->offer.kind);
                }
            } else if (fixture.minimumPlayers <= 1) {
                CHECK_FALSE(reason.empty());
                unsupported.insert(fixture.resolvedTemplateKey);
            }
        }
    }
    CHECK(maps == 34);
    CHECK(placements == 1199);
    CHECK(kinds.contains(static_cast<s32>(ItemKind::Gold)));
    CHECK(kinds.contains(static_cast<s32>(ItemKind::Keys)));
    CHECK(kinds.contains(static_cast<s32>(ItemKind::Food)));
    CHECK(kinds.contains(static_cast<s32>(ItemKind::Potion)));
    CHECK(kinds.contains(static_cast<s32>(ItemKind::WeaponPowerup)));
    CHECK(kinds.contains(static_cast<s32>(ItemKind::ArmorPowerup)));
    CHECK(kinds.contains(static_cast<s32>(ItemKind::SpecialPowerup)));
    CHECK(kinds.contains(static_cast<s32>(ItemKind::Scroll)));
    CHECK(awards.contains(LegendsRelic::Runestone));
    CHECK(awards.contains(LegendsRelic::LegendItem));
    // Explicitly bounded native backlog: stat-saturating boots, Death actors,
    // character coins, scrolls, and combined key/potion bundles.
    for (const u16 key : unsupported) {
        INFO(key);
        const u16 kind = key >> 8U;
        CHECK((key == 0x0200 || kind == 0x21 || kind == 0x27 || kind == 0x28 || kind == 0x2f));
    }
}
