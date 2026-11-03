#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/characters/Inventory.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/console/GiftRequest.h"
#include "game/players/ItemPickup.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Args = std::vector<std::string>;
using Kind = GiftRequest::Kind;

/** What get answers words it cannot read. */
constexpr std::string_view kGetUsage =
    "get <item> [amount]   keys, potions, gold, health or a powerup\n"
    "get exp <points>      experience to add\n"
    "get level <level>     the level to be, 1 to 99\n"
    "get info [$SELECTED]  what the thing clicked in the view is\n"
    "list items names the items.";

/** Words for get and what reading them says. */
struct Refusal {
    Args words;
    std::string_view error;
};

TEST_CASE("get reads the item, its amount and a potion's kind", "[dev-console][commands]") {
    const auto get = [](const Args& args) {
        const auto gift = parseGift(args);
        REQUIRE(gift.has_value());
        return *gift;
    };
    auto key = get({"key"});
    CHECK(key.kind == Kind::Item);
    CHECK(key.offer.kind == static_cast<s32>(ItemKind::Keys));
    CHECK(key.offer.amount == GiftRequest::kDefaultKeys);
    CHECK(get({"KEYS", "3"}).offer.amount == 3);
    auto potion = get({"potion"});
    CHECK(potion.offer.kind == static_cast<s32>(ItemKind::Potion));
    CHECK(potion.offer.amount == 1);
    CHECK(potion.offer.flags == static_cast<u32>(GiftRequest::kDefaultPotionKind));
    auto acid = get({"potions", "2", "acid"});
    CHECK(acid.offer.amount == 2);
    CHECK(acid.offer.flags == 4);
    auto lightning = get({"potion", "lightning"});
    CHECK(lightning.offer.amount == 1);
    CHECK(lightning.offer.flags == 3);
    CHECK(get({"potion", "electric", "5"}).offer.flags == 2);
    auto gold = get({"gold"});
    CHECK(gold.offer.kind == static_cast<s32>(ItemKind::Gold));
    CHECK(gold.offer.amount == GiftRequest::kDefaultGold);
    auto health = get({"health", "50"});
    CHECK(health.offer.kind == static_cast<s32>(ItemKind::Food));
    CHECK(health.offer.amount == 50);
    auto wings = get({"levitate"});
    CHECK(wings.offer.kind == powerup::kSpecial);
    CHECK(wings.offer.flags == powerup::kLevitation);
    CHECK(wings.offer.strength == Approx(GiftRequest::kDefaultSeconds));
    CHECK(wings.name == "levitate");
    CHECK(get({"LEVITATE", "-1"}).offer.strength == Approx(-1.0f));
    CHECK(get({"invulnerable", "90"}).offer.strength == Approx(90.0f));
    auto shots = get({"supershot"});
    CHECK(shots.offer.kind == powerup::kWeapon);
    CHECK(shots.offer.flags == damage::kSuperShot);
    CHECK(shots.offer.amount > 0); // charges: one huge shot each
    CHECK(get({"supershot", "5"}).offer.amount == 5);
    auto halo = get({"antideath"});
    CHECK(halo.offer.kind == powerup::kArmor);
    CHECK(halo.offer.flags == 0x80000);
    auto exp = get({"exp", "500"});
    CHECK(exp.kind == Kind::Experience);
    CHECK(exp.amount == 500);
    CHECK(get({"experience", "7"}).amount == 7);
    auto level = get({"level", "42"});
    CHECK(level.kind == Kind::Level);
    CHECK(level.amount == 42);
    const auto none = parseGift(Args{});
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error() == giftUsage());
    const auto sword = parseGift(Args{"sword"});
    REQUIRE_FALSE(sword.has_value());
    CHECK(sword.error().contains("sword"));
    CHECK_FALSE(parseGift(Args{"exp"}).has_value());
    CHECK_FALSE(parseGift(Args{"exp", "-5"}).has_value());
    CHECK_FALSE(parseGift(Args{"level", "0"}).has_value());
    CHECK_FALSE(parseGift(Args{"level", "100"}).has_value());
    CHECK_FALSE(parseGift(Args{"key", "x"}).has_value());
    CHECK_FALSE(parseGift(Args{"key", "0"}).has_value());
    CHECK_FALSE(parseGift(Args{"potion", "slime"}).has_value());
}

TEST_CASE("get says what came of the gift", "[dev-console][commands]") {
    const auto keys = parseGift(Args{"keys", "3"});
    REQUIRE(keys.has_value());
    ItemTaking taking;
    taking.outcome = ItemTaking::Outcome::Taken;
    taking.count = 3;
    CHECK(giftReport(*keys, taking) == "Got 3 keys");
    taking.outcome = ItemTaking::Outcome::PartTaken;
    taking.count = 2;
    taking.left = 1;
    CHECK(giftReport(*keys, taking) == "Got 2 keys; no room for 1 more");
    taking.outcome = ItemTaking::Outcome::KeysFull;
    CHECK(giftReport(*keys, taking) == "No room for more keys");
    const auto wings = parseGift(Args{"levitate", "-1"});
    REQUIRE(wings.has_value());
    taking.outcome = ItemTaking::Outcome::Taken;
    CHECK(giftReport(*wings, taking) == "Got levitate for good");
    const auto timed = parseGift(Args{"levitate", "45"});
    REQUIRE(timed.has_value());
    CHECK(giftReport(*timed, taking) == "Got levitate for 45 seconds");
    taking.outcome = ItemTaking::Outcome::NotCarried;
    CHECK(giftReport(*wings, taking) == "Cannot carry levitate");
}

TEST_CASE("held powerups and potions are named the way get names them", "[dev-console][commands]") {
    const PowerupSlot wings{
        .strength = 30.0f, .kind = powerup::kSpecial, .flags = powerup::kLevitation};
    CHECK(powerupName(wings) == "levitate");
    const PowerupSlot giant{.strength = 30.0f,
                            .kind = powerup::kSpecial,
                            .flags = powerup::kGrowth | powerup::kEnemyShrink};
    CHECK(powerupName(giant) == "growth+shrink");
    const PowerupSlot odd{.strength = 30.0f, .kind = powerup::kMagic, .flags = 0x12345};
    CHECK(powerupName(odd).contains("kind 8"));
    CHECK(potionName(0) == "fire");
    CHECK(potionName(1) == "fire");
    CHECK(potionName(2) == "electric");
    CHECK(potionName(3) == "lightning");
    CHECK(potionName(4) == "acid");
    CHECK(potionName(9) == "fire");
}

TEST_CASE("get info asks after the thing selected, or one named by its token",
          "[dev-console][commands]") {
    const auto selected = parseGift(Args{"info"});
    REQUIRE(selected.has_value());
    CHECK(selected->kind == Kind::Info);
    CHECK(selected->name.empty());
    const auto named = parseGift(Args{"INFO", "object#3"});
    REQUIRE(named.has_value());
    CHECK(named->kind == Kind::Info);
    CHECK(named->name == "object#3");
    const auto more = parseGift(Args{"info", "object#3", "more"});
    REQUIRE_FALSE(more.has_value());
    CHECK(more.error().find("get info") != std::string::npos);
    CHECK(giftUsage().find("get info [$SELECTED]") != std::string::npos);
}

TEST_CASE("get's usage reads as it does today", "[dev-console][commands]") {
    CHECK(giftUsage() == kGetUsage);
}

TEST_CASE("get refuses the words it cannot read as it does today", "[dev-console][commands]") {
    for (const Refusal& refusal :
         {Refusal{{}, kGetUsage},
          Refusal{{"info", "a", "b"}, "get info [$SELECTED]: one thing, or what is selected"},
          Refusal{{"exp"}, "get exp <points>: how much to add"},
          Refusal{{"exp", "-5"}, "get exp <points>: how much to add"},
          Refusal{{"level", "0"}, "get level <level>: 1 to 99"},
          Refusal{{"level", "100"}, "get level <level>: 1 to 99"}, Refusal{{"key", "x"}, kGetUsage},
          Refusal{{"key", "0"}, kGetUsage}, Refusal{{"gold", "1", "2"}, kGetUsage},
          Refusal{{"potion", "slime"},
                  "No potion kind named slime; fire, electric, lightning or acid"},
          Refusal{{"sword"}, "No item named sword; list items names them"},
          Refusal{{"supershot", "x"}, kGetUsage},
          Refusal{{"levitate", "0"}, "get levitate [seconds]: how long for; under nought for good"},
          Refusal{{"levitate", "x"},
                  "get levitate [seconds]: how long for; under nought for good"}}) {
        CAPTURE(refusal.words);
        const auto gift = parseGift(refusal.words);
        CHECK_FALSE(gift.has_value());
        if (!gift.has_value()) {
            CHECK(gift.error() == refusal.error);
        }
    }
}

TEST_CASE("get says what came of a full hand, a held item, a shown one and charges as today",
          "[dev-console][commands]") {
    const auto report = [](const Args& words, ItemTaking::Outcome outcome, s32 count) {
        const auto gift = parseGift(words);
        REQUIRE(gift.has_value());
        ItemTaking taking;
        taking.outcome = outcome;
        taking.count = count;
        return giftReport(*gift, taking);
    };
    CHECK(report({"health"}, ItemTaking::Outcome::HealthFull, 0) == "Health is full already");
    CHECK(report({"gold"}, ItemTaking::Outcome::GoldFull, 0) == "No room for more gold");
    CHECK(report({"potions"}, ItemTaking::Outcome::PotionsFull, 0) == "No room for more potions");
    CHECK(report({"levitate"}, ItemTaking::Outcome::AlreadyHeld, 0) == "Already holding levitate");
    CHECK(report({"key"}, ItemTaking::Outcome::Shown, 1) == "Got 1 key");
    CHECK(report({"supershot"}, ItemTaking::Outcome::Taken, 1) == "Got supershot, 10 charges");
}

} // namespace
