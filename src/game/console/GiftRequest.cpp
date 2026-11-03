#include "game/console/GiftRequest.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <utility>

#include "game/characters/Progression.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/console/ConsoleArguments.h"
#include "game/console/ConsoleUsage.h"
#include "game/enemies/DeathRules.h"

namespace gdl::game {

namespace {
constexpr f32 kForGood = -1.0f; ///< a powerup's strength under none lasts

struct Potion {
    std::string_view name;
    s32 kind;
};
/** The kinds the shop sells, by the colour each burst takes. */
constexpr std::array<Potion, 4> kPotions{
    {{"fire", 1}, {"electric", 2}, {"lightning", 3}, {"acid", 4}}};

constexpr std::array<GiftPowerup, 28> kPowerups{{
    {"levitate", "held above the ground", powerup::kSpecial, powerup::kLevitation, false, 0},
    {"invulnerable", "takes no harm", powerup::kArmor, armor::kInvulnerable, false, 0},
    {"goldinvulnerable", "takes no harm, gilded", powerup::kArmor, armor::kGoldInvulnerable, false,
     0},
    {"antideath", "a halo Death cannot take", powerup::kArmor, DeathRules::kProtection, false, 0},
    {"reflectshield", "turns missiles back", powerup::kArmor, armor::kReflect, false, 0},
    {"fireshield", "burns what it is against", powerup::kArmor, armor::kFireArmor, false, 0},
    {"lightningshield", "shocks what it is against", powerup::kArmor, armor::kElectricArmor, false,
     0},
    {"supershot", "one huge shot a charge", powerup::kWeapon, damage::kSuperShot, true, 0},
    {"threeway", "three shots at once", powerup::kWeapon, damage::kThreeWayShot, false, 0},
    {"fiveway", "five shots at once", powerup::kWeapon, damage::kFiveWayShot, false, 0},
    {"rapidfire", "shoots faster", powerup::kWeapon, damage::kRapidFire, false, 0},
    {"reflect", "shots that bounce off the walls", powerup::kWeapon, damage::kReflect, false, 0},
    {"thunderhammer", "a hammer blow of lightning a charge", powerup::kWeapon, damage::kHammer,
     true, 0},
    {"firebreath", "breathes fire a charge", powerup::kSpecial, powerup::kFireBreath, true, 0},
    {"acidbreath", "breathes acid a charge", powerup::kSpecial, powerup::kAcidBreath, true, 0},
    {"lightningbreath", "breathes lightning a charge", powerup::kSpecial, powerup::kLightningBreath,
     true, 0},
    {"phoenix", "a fire-spitting companion", powerup::kSpecial, powerup::kPhoenix, false, 0},
    {"growth", "grows the character", powerup::kSpecial, powerup::kGrowth, false, 0},
    {"shrink", "shrinks the swarm", powerup::kSpecial, powerup::kEnemyShrink, false, 0},
    {"invisible", "unseen by the enemies", powerup::kSpecial, powerup::kInvisible, false, 0},
    {"xray", "sees inside the nearest closed chest", powerup::kSpecial, powerup::kXRay, false, 0},
    {"stoptime", "halts ordinary opponents and hazards", powerup::kSpecial, powerup::kStopTime,
     false, 0},
    {"turbo", "fills the turbo meter at once", powerup::kSpecial, powerup::kTurbo, false, 0},
    {"speed", "runs faster", powerup::kSpecial, powerup::kSpeedBoost, false, 4},
    {"mikey", "a decoy the swarm chases", powerup::kSpecial, powerup::kMikey, false, 0},
    {"handofdeath", "returns melee harm to the striker", powerup::kSpecial, powerup::kHandOfDeath,
     false, 0},
    {"healthvamp", "melee harm heals the character", powerup::kSpecial, powerup::kHealthVamp, false,
     0},
    {"pojo", "Pojo speaks for the character", powerup::kSpecial, powerup::kPojo, false, 0},
}};

const GiftPowerup* powerupNamed(std::string_view name) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kPowerups, name, &GiftPowerup::name);
    return found != kPowerups.end() ? &*found : nullptr;
}

const Potion* potionNamed(std::string_view name) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kPotions, name, &Potion::name);
    return found != kPotions.end() ? &*found : nullptr;
}

/** A positive count from the one optional argument, else `fallback`; the usage on nonsense. */
gdl::Expected<s32, std::string> countOf(ConsoleCommand::Arguments rest, s32 fallback) {
    if (rest.empty()) {
        return fallback;
    }
    const auto value = rest.size() == 1 ? integerArgument(rest[0]) : std::nullopt;
    if (!value.has_value() || *value <= 0) {
        return gdl::Unexpected(giftUsage());
    }
    return *value;
}

/** The name as a count of it reads: one key, two keys. */
std::string counted(const GiftRequest& gift, s32 count) {
    if (gift.name == "keys" || gift.name == "potions") {
        return count == 1 ? gift.name.substr(0, gift.name.size() - 1) : gift.name;
    }
    return gift.name;
}
} // namespace

std::string giftUsage() {
    const std::string level = std::format("the level to be, 1 to {}", kMaxLevel);
    const std::array<std::pair<std::string_view, std::string_view>, 4> rows{
        {{"get <item> [amount]", "keys, potions, gold, health or a powerup"},
         {"get exp <points>", "experience to add"},
         {"get level <level>", level},
         {"get info [$SELECTED]", "what the thing clicked in the view is"}}};
    constexpr std::array<std::string_view, 1> kNotes{"list items names the items."};
    return consoleUsage(rows, kNotes);
}

gdl::Expected<GiftRequest, std::string> parseGift(ConsoleCommand::Arguments arguments) {
    if (arguments.empty()) {
        return gdl::Unexpected(giftUsage());
    }
    const std::string name = lowerArgument(arguments[0]);
    const ConsoleCommand::Arguments rest = arguments.subspan(1);
    GiftRequest gift;
    if (name == "info") {
        if (rest.size() > 1) {
            return gdl::Unexpected(
                std::string("get info [$SELECTED]: one thing, or what is selected"));
        }
        gift.kind = GiftRequest::Kind::Info;
        gift.name = rest.empty() ? "" : rest[0];
        return gift;
    }
    if (name == "exp" || name == "experience") {
        const auto points = rest.size() == 1 ? integerArgument(rest[0]) : std::nullopt;
        if (!points.has_value() || *points <= 0) {
            return gdl::Unexpected(std::string("get exp <points>: how much to add"));
        }
        gift.kind = GiftRequest::Kind::Experience;
        gift.amount = *points;
        gift.name = "exp";
        return gift;
    }
    if (name == "level") {
        const auto level = rest.size() == 1 ? integerArgument(rest[0]) : std::nullopt;
        if (!level.has_value() || *level < 1 || *level > kMaxLevel) {
            return gdl::Unexpected(std::format("get level <level>: 1 to {}", kMaxLevel));
        }
        gift.kind = GiftRequest::Kind::Level;
        gift.amount = *level;
        gift.name = "level";
        return gift;
    }
    if (name == "key" || name == "keys") {
        const auto count = countOf(rest, GiftRequest::kDefaultKeys);
        if (!count) {
            return gdl::Unexpected(count.error());
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Keys), *count};
        gift.name = "keys";
        return gift;
    }
    if (name == "potion" || name == "potions") {
        s32 count = GiftRequest::kDefaultPotions;
        s32 kind = GiftRequest::kDefaultPotionKind;
        for (const std::string& argument : rest) {
            if (const auto value = integerArgument(argument); value.has_value() && *value > 0) {
                count = *value;
            } else if (const Potion* potion = potionNamed(lowerArgument(argument))) {
                kind = potion->kind;
            } else {
                return gdl::Unexpected(std::format(
                    "No potion kind named {}; fire, electric, lightning or acid", argument));
            }
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Potion), count, static_cast<u32>(kind)};
        gift.name = "potions";
        return gift;
    }
    if (name == "gold") {
        const auto count = countOf(rest, GiftRequest::kDefaultGold);
        if (!count) {
            return gdl::Unexpected(count.error());
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Gold), *count};
        gift.name = "gold";
        return gift;
    }
    if (name == "health" || name == "food") {
        const auto count = countOf(rest, GiftRequest::kDefaultHealth);
        if (!count) {
            return gdl::Unexpected(count.error());
        }
        gift.offer = ItemOffer{static_cast<s32>(ItemKind::Food), *count};
        gift.name = "health";
        return gift;
    }
    const GiftPowerup* powerup = powerupNamed(name);
    if (powerup == nullptr) {
        return gdl::Unexpected(
            std::format("No item named {}; list items names them", arguments[0]));
    }
    gift.name = std::string(powerup->name);
    if (powerup->charged) {
        const auto charges = countOf(rest, GiftRequest::kDefaultCharges);
        if (!charges) {
            return gdl::Unexpected(charges.error());
        }
        gift.offer = ItemOffer{powerup->kind, *charges, powerup->flags, kForGood};
        return gift;
    }
    f32 seconds = GiftRequest::kDefaultSeconds;
    if (!rest.empty()) {
        const auto given = rest.size() == 1 ? numberArgument(rest[0]) : std::nullopt;
        if (!given.has_value() || *given == 0.0f) {
            return gdl::Unexpected(std::format(
                "get {} [seconds]: how long for; under nought for good", powerup->name));
        }
        seconds = *given < 0.0f ? kForGood : *given;
    }
    gift.offer =
        ItemOffer{powerup->kind, static_cast<s32>(powerup->charge), powerup->flags, seconds};
    return gift;
}

std::string giftReport(const GiftRequest& gift, const ItemTaking& taking) {
    const GiftPowerup* powerup = powerupNamed(gift.name);
    switch (taking.outcome) {
    case ItemTaking::Outcome::Taken:
    case ItemTaking::Outcome::Shown:
        if (powerup == nullptr) {
            return std::format("Got {} {}", taking.count, counted(gift, taking.count));
        }
        if (powerup->charged) {
            return std::format("Got {}, {} charges", gift.name, gift.offer.amount);
        }
        if (gift.offer.strength < 0.0f) {
            return std::format("Got {} for good", gift.name);
        }
        return std::format("Got {} for {:.0f} seconds", gift.name, gift.offer.strength);
    case ItemTaking::Outcome::PartTaken:
        return std::format("Got {} {}; no room for {} more", taking.count,
                           counted(gift, taking.count), taking.left);
    case ItemTaking::Outcome::HealthFull: return "Health is full already";
    case ItemTaking::Outcome::GoldFull:
    case ItemTaking::Outcome::KeysFull:
    case ItemTaking::Outcome::PotionsFull: return std::format("No room for more {}", gift.name);
    case ItemTaking::Outcome::AlreadyHeld: return std::format("Already holding {}", gift.name);
    case ItemTaking::Outcome::NotCarried: break;
    }
    return std::format("Cannot carry {}", gift.name);
}

std::span<const GiftPowerup> giftPowerups() {
    return kPowerups;
}

std::string_view potionName(s32 kind) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kPotions, kind, &Potion::kind);
    return found != kPotions.end() ? found->name : kPotions.front().name;
}

std::string powerupName(const PowerupSlot& slot) {
    std::string names;
    for (const GiftPowerup& powerup : kPowerups) {
        if (powerup.kind == slot.kind && (slot.flags & powerup.flags) == powerup.flags) {
            names += names.empty() ? "" : "+";
            names += powerup.name;
        }
    }
    return names.empty() ? std::format("kind {} flags {:#x}", slot.kind, slot.flags) : names;
}

} // namespace gdl::game
