#include "expansions/n64/hud/LegendsN64HudIcons.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/characters/ClassData.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/hud/StatusBox.h"

namespace gdl::game {
namespace {

/** Dark Legacy's potion kinds, as StatusBoxPainter::kPotionIcons orders them, as N64 bottles. */
constexpr std::array<std::string_view, 5> kBottles{"POTIONR", "POTIONR", "POTIONB", "POTIONY",
                                                   "POTIONG"};
static_assert(kBottles.size() == StatusBoxPainter::kPotionIcons.size());

/** A Dark Legacy class the N64 draws: its portraits' bank and code (FUN_e006b110's banks,
 * e005a2f0 and e005a318's names) and its word (e005a288). */
struct NativeClass {
    s32 classIndex = 0;
    std::string_view bank;
    std::string_view code;
    s32 text = 0;
};

constexpr std::array kNativeClasses{
    NativeClass{kWarriorClass, "hud/warf", "WAR", 129},
    NativeClass{kValkyrieClass, "hud/valf", "VAL", 132},
    NativeClass{kWizardClass, "hud/wizf", "WIZ", 130},
    NativeClass{kArcherClass, "hud/arcf", "ARC", 131},
    NativeClass{kMinotaurClass, "hud/minf", "MIN", 134},
    NativeClass{kFalconessClass, "hud/falf", "FAL", 137},
    NativeClass{kJackalClass, "hud/jacf", "JAC", 135},
    NativeClass{kTigressClass, "hud/tigf", "TIG", 136},
    NativeClass{kSumnerClass, "hud/sumf", "SUM", 138},
};

/** The N64's word for its class 4, which Dark Legacy has no class for: a character carrying
 * Pojo is named by it, as Dark Legacy's messages name such a character Pojo. */
constexpr s32 kPojoText = 133;

const NativeClass* nativeClassOf(s32 classIndex) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kNativeClasses, classIndex, &NativeClass::classIndex);
    return found == kNativeClasses.end() ? nullptr : &*found;
}

/** One of Dark Legacy's powerups, by its kind and what it carries, and what the carousel shows
 * it by: the N64's icon of the same powerup (game.bin's item table, e00560a4), else Dark
 * Legacy's own picture. */
struct PowerupPicture {
    s32 kind = 0;
    u32 flags = 0;   ///< what the powerup carries, every one of it; none for the rows below
    u32 element = 0; ///< an amulet's element, 1 to 4; none for the rest
    std::string_view picture;
    bool lent = false; ///< the picture is Dark Legacy's INVENTORY one, not the HUD bank's
};

/** Every powerup of Dark Legacy's levels and shop. Those that carry flags are tried in turn,
 * then the amulets, then a kind's own. */
constexpr std::array<PowerupPicture, 38> kPowerupPictures{{
    {.kind = powerup::kWeapon, .flags = damage::kThreeWayShot, .picture = "HUD_3WAY"},
    {.kind = powerup::kWeapon, .flags = damage::kFiveWayShot, .picture = "HUD_5WAY"},
    {.kind = powerup::kWeapon, .flags = damage::kSuperShot, .picture = "HUD_SUPERSHOT"},
    {.kind = powerup::kWeapon, .flags = damage::kReflect, .picture = "HUD_REFLSHOT"},
    {.kind = powerup::kWeapon, .flags = damage::kHammer, .picture = "HUD_HAMMER"},
    {.kind = powerup::kWeapon, .flags = damage::kRapidFire, .picture = "HUD_RAPIDFIRE"},
    {.kind = powerup::kWeapon, .element = damage::kFire, .picture = "HUD_FIREAMLT"},
    {.kind = powerup::kWeapon, .element = damage::kElectric, .picture = "HUD_ELECAMLT"},
    {.kind = powerup::kWeapon, .element = damage::kLight, .picture = "HUD_LITEAMLT"},
    {.kind = powerup::kWeapon, .element = damage::kAcid, .picture = "HUD_ACIDAMLT"},
    // Gold invulnerability carries silver's flag too, so it is tried first.
    {.kind = powerup::kArmor, .flags = armor::kGoldInvulnerable, .picture = "HUD_INVULNG"},
    {.kind = powerup::kArmor, .flags = armor::kInvulnerable, .picture = "HUD_INVULN"},
    {.kind = powerup::kArmor, .flags = armor::kAntiDeath, .picture = "HUD_HALO"},
    {.kind = powerup::kArmor, .flags = armor::kReflect, .picture = "HUD_REFLSHIELD"},
    {.kind = powerup::kArmor, .flags = armor::kFireArmor, .picture = "HUD_FIRESHIELD"},
    {.kind = powerup::kArmor, .flags = armor::kElectricArmor, .picture = "HUD_ELECSHIELD"},
    {.kind = powerup::kArmor,
     .flags = armor::kResistAcid | armor::kImmuneGas,
     .picture = "SHP_GASMASK",
     .lent = true},
    {.kind = powerup::kSpeed, .picture = "HUD_SPEEDBOOST"},
    {.kind = powerup::kSpecial, .flags = powerup::kSpeedBoost, .picture = "HUD_SPEEDBOOST"},
    {.kind = powerup::kSpecial, .flags = powerup::kLevitation, .picture = "HUD_LEVITATE"},
    {.kind = powerup::kSpecial,
     .flags = powerup::kXRay,
     .picture = "SHP_XRAY_GLASSES",
     .lent = true},
    {.kind = powerup::kSpecial, .flags = powerup::kInvisible, .picture = "HUD_INVIS"},
    {.kind = powerup::kSpecial, .flags = powerup::kStopTime, .picture = "HUD_TIMESTOP"},
    {.kind = powerup::kSpecial, .flags = powerup::kFireBreath, .picture = "HUD_FIREBRETH"},
    {.kind = powerup::kSpecial, .flags = powerup::kAcidBreath, .picture = "HUD_ACIDBRETH"},
    {.kind = powerup::kSpecial, .flags = powerup::kLightningBreath, .picture = "HUD_ELECBRETH"},
    {.kind = powerup::kSpecial, .flags = powerup::kPhoenix, .picture = "HUD_PHOENIX"},
    {.kind = powerup::kSpecial, .flags = powerup::kGrowth, .picture = "HUD_GROWTH"},
    {.kind = powerup::kSpecial, .flags = powerup::kEnemyShrink, .picture = "HUD_SHRINK"},
    {.kind = powerup::kSpecial, .flags = powerup::kPojo, .picture = "SHP_POJOEGG", .lent = true},
    {.kind = powerup::kSpecial, .flags = powerup::kSkorneHorns, .picture = "HUD_SHORN"},
    {.kind = powerup::kSpecial, .flags = powerup::kSkorneMask, .picture = "HUD_SMASK"},
    {.kind = powerup::kSpecial, .flags = powerup::kRightGauntlet, .picture = "HUD_SGAUNTR"},
    // The N64 draws its left gauntlet (0xa50) with the right one's icon too.
    {.kind = powerup::kSpecial, .flags = powerup::kLeftGauntlet, .picture = "HUD_SGAUNTR"},
    {.kind = powerup::kSpecial,
     .flags = powerup::kTurbo,
     .picture = "SHP_TURBO_BOOST",
     .lent = true},
    {.kind = powerup::kSpecial, .flags = powerup::kMikey, .picture = "SHP_MIKEY", .lent = true},
    {.kind = powerup::kSpecial,
     .flags = powerup::kHandOfDeath,
     .picture = "SHP_HANDOFDEATH",
     .lent = true},
    {.kind = powerup::kSpecial,
     .flags = powerup::kHealthVamp,
     .picture = "SHP_HEALTHVAMPIRE",
     .lent = true},
}};

const PowerupPicture* pictureOf(s32 kind, u32 flags) {
    const auto first = [](auto matches) -> const PowerupPicture* {
        // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
        // NOLINTNEXTLINE(readability-qualified-auto)
        const auto found = std::ranges::find_if(kPowerupPictures, matches);
        return found == kPowerupPictures.end() ? nullptr : &*found;
    };
    if (const PowerupPicture* flagged = first([&](const PowerupPicture& row) {
            return row.kind == kind && row.flags != 0 && (flags & row.flags) == row.flags;
        })) {
        return flagged;
    }
    // An amulet keeps its element in the weapon's low nibble, not in flag bits.
    const u32 element = flags & damage::kElementMask;
    if (kind == powerup::kWeapon && element != 0) {
        if (const PowerupPicture* amulet = first([&](const PowerupPicture& row) {
                return row.kind == kind && row.element == element;
            })) {
            return amulet;
        }
    }
    return first([&](const PowerupPicture& row) {
        return row.kind == kind && row.flags == 0 && row.element == 0;
    });
}

/** The picture of `row`, a table's row or none, when it is the HUD bank's (`lent` false) or
 * Dark Legacy's (`lent` true). */
template <class Row> std::optional<std::string_view> pictureIn(const Row* row, bool lent) {
    return row != nullptr && row->lent == lent ? std::optional{row->picture} : std::nullopt;
}

/** A legend item's realm and what the quest page shows it by: the N64's icon of the same item
 * (e00560a4), else Dark Legacy's own picture of it. */
struct LegendPicture {
    s32 realm = 0;
    std::string_view picture;
    bool lent = false; ///< the picture is Dark Legacy's SELECT one, not the HUD bank's
};

constexpr std::array<LegendPicture, kLegendsN64LegendRealms.size()> kLegendPictures{{
    {.realm = 1, .picture = "HUD_SCIMITAR"},
    {.realm = 2, .picture = "HUD_ICEAXE"},
    {.realm = 3, .picture = "LAMP", .lent = true},
    {.realm = 4, .picture = "BILLOWS", .lent = true},
    {.realm = 5, .picture = "HUD_SAVIOUR"},
    {.realm = 7, .picture = "BOOK", .lent = true},
    {.realm = 9, .picture = "HUD_FLAMESCROLL"},
    {.realm = 10, .picture = "LANTERN", .lent = true},
    {.realm = 11, .picture = "HUD_JAVELIN"},
}};

constexpr bool legendRealmsAgree() {
    for (usize index = 0; index < kLegendPictures.size(); ++index) {
        if (kLegendPictures.at(index).realm != kLegendsN64LegendRealms.at(index)) {
            return false;
        }
    }
    return true;
}
static_assert(legendRealmsAgree());

const LegendPicture* legendOf(s32 realm) {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find(kLegendPictures, realm, &LegendPicture::realm);
    return found == kLegendPictures.end() ? nullptr : &*found;
}

/** Adds `name` to `names` unless it is there already. */
void addOnce(std::vector<std::string_view>& names, std::string_view name) {
    if (std::ranges::find(names, name) == names.end()) {
        names.push_back(name);
    }
}

} // namespace

std::string_view legendsN64PotionBottle(s32 kind) {
    return kind >= 0 && static_cast<usize>(kind) < kBottles.size()
               ? kBottles[static_cast<usize>(kind)]
               : kBottles[0];
}

std::optional<std::string_view> legendsN64ClassBank(s32 classIndex) {
    const NativeClass* found = nativeClassOf(classIndex);
    return found != nullptr ? std::optional{found->bank} : std::nullopt;
}

std::optional<std::string_view> legendsN64ClassCode(s32 classIndex) {
    const NativeClass* found = nativeClassOf(classIndex);
    return found != nullptr ? std::optional{found->code} : std::nullopt;
}

std::optional<s32> legendsN64ClassText(s32 classIndex, bool pojo) {
    if (pojo) {
        return kPojoText;
    }
    const NativeClass* found = nativeClassOf(classIndex);
    return found != nullptr ? std::optional{found->text} : std::nullopt;
}

std::optional<std::string_view> legendsN64PowerupIcon(s32 kind, u32 flags) {
    return pictureIn(pictureOf(kind, flags), false);
}

std::optional<std::string_view> legendsN64PowerupFallback(s32 kind, u32 flags) {
    return pictureIn(pictureOf(kind, flags), true);
}

std::optional<std::string_view> legendsN64LegendIcon(s32 realm) {
    return pictureIn(legendOf(realm), false);
}

std::optional<std::string_view> legendsN64LegendFallback(s32 realm) {
    return pictureIn(legendOf(realm), true);
}

std::vector<std::string_view> legendsN64ItemIcons() {
    std::vector<std::string_view> names;
    for (const PowerupPicture& row : kPowerupPictures) {
        if (!row.lent) {
            addOnce(names, row.picture);
        }
    }
    for (const LegendPicture& row : kLegendPictures) {
        if (!row.lent) {
            addOnce(names, row.picture);
        }
    }
    return names;
}

std::vector<std::string_view> legendsN64PowerupFallbacks() {
    std::vector<std::string_view> names;
    for (const PowerupPicture& row : kPowerupPictures) {
        if (row.lent) {
            addOnce(names, row.picture);
        }
    }
    return names;
}

} // namespace gdl::game
