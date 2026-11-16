#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "expansions/n64/hud/LegendsN64HudIcons.h"
#include "game/characters/ClassData.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/hud/StatusBox.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** One of Dark Legacy's powerups as its levels place it or its shop sells it, and what the N64
 * carousel shows it by: the N64's icon of the same item (game.bin e00560a4), else Dark
 * Legacy's INVENTORY picture. */
struct Pair {
    s32 kind = 0;
    u32 flags = 0;
    std::string_view picture;
    bool lent = false; ///< Dark Legacy's picture, not the N64's icon
};

constexpr std::array<Pair, 38> kPairs{{
    {.kind = powerup::kWeapon, .flags = damage::kFire, .picture = "HUD_FIREAMLT"},
    {.kind = powerup::kWeapon, .flags = damage::kElectric, .picture = "HUD_ELECAMLT"},
    {.kind = powerup::kWeapon, .flags = damage::kLight, .picture = "HUD_LITEAMLT"},
    {.kind = powerup::kWeapon, .flags = damage::kAcid, .picture = "HUD_ACIDAMLT"},
    {.kind = powerup::kWeapon, .flags = 0x80000, .picture = "HUD_3WAY"},
    {.kind = powerup::kWeapon, .flags = 0x400000, .picture = "HUD_5WAY"},
    {.kind = powerup::kWeapon, .flags = 0x100000, .picture = "HUD_SUPERSHOT"},
    {.kind = powerup::kWeapon, .flags = 0x200000, .picture = "HUD_REFLSHOT"},
    {.kind = powerup::kWeapon, .flags = 0x10000000, .picture = "HUD_HAMMER"},
    {.kind = powerup::kWeapon, .flags = 0x20000000, .picture = "HUD_RAPIDFIRE"},
    {.kind = powerup::kArmor, .flags = 0x110000, .picture = "HUD_INVULNG"},
    {.kind = powerup::kArmor, .flags = 0x10000, .picture = "HUD_INVULN"},
    {.kind = powerup::kArmor, .flags = 0x80000, .picture = "HUD_HALO"},
    {.kind = powerup::kArmor, .flags = 0x20000, .picture = "HUD_REFLSHIELD"},
    {.kind = powerup::kArmor, .flags = 0x200000, .picture = "HUD_FIRESHIELD"},
    {.kind = powerup::kArmor, .flags = 0x400000, .picture = "HUD_ELECSHIELD"},
    {.kind = powerup::kArmor, .flags = 0x2008, .picture = "SHP_GASMASK", .lent = true},
    {.kind = powerup::kSpeed, .flags = 0, .picture = "HUD_SPEEDBOOST"},
    {.kind = powerup::kSpecial, .flags = 0x10000, .picture = "HUD_SPEEDBOOST"},
    {.kind = powerup::kSpecial, .flags = 0x1, .picture = "HUD_LEVITATE"},
    {.kind = powerup::kSpecial, .flags = 0x2, .picture = "SHP_XRAY_GLASSES", .lent = true},
    {.kind = powerup::kSpecial, .flags = 0x4, .picture = "HUD_INVIS"},
    {.kind = powerup::kSpecial, .flags = 0x8, .picture = "HUD_TIMESTOP"},
    {.kind = powerup::kSpecial, .flags = 0x10, .picture = "HUD_FIREBRETH"},
    {.kind = powerup::kSpecial, .flags = 0x20, .picture = "HUD_ACIDBRETH"},
    {.kind = powerup::kSpecial, .flags = 0x40, .picture = "HUD_ELECBRETH"},
    {.kind = powerup::kSpecial, .flags = 0x80, .picture = "HUD_PHOENIX"},
    {.kind = powerup::kSpecial, .flags = 0x100, .picture = "HUD_GROWTH"},
    {.kind = powerup::kSpecial, .flags = 0x200, .picture = "HUD_SHRINK"},
    {.kind = powerup::kSpecial, .flags = 0x400, .picture = "SHP_POJOEGG", .lent = true},
    {.kind = powerup::kSpecial, .flags = 0x1000, .picture = "HUD_SHORN"},
    {.kind = powerup::kSpecial, .flags = 0x2000, .picture = "HUD_SMASK"},
    {.kind = powerup::kSpecial, .flags = 0x4000, .picture = "HUD_SGAUNTR"},
    {.kind = powerup::kSpecial, .flags = 0x8000, .picture = "HUD_SGAUNTR"},
    {.kind = powerup::kSpecial, .flags = 0x80000, .picture = "SHP_TURBO_BOOST", .lent = true},
    {.kind = powerup::kSpecial, .flags = 0x100000, .picture = "SHP_MIKEY", .lent = true},
    {.kind = powerup::kSpecial, .flags = 0x200000, .picture = "SHP_HANDOFDEATH", .lent = true},
    {.kind = powerup::kSpecial, .flags = 0x400000, .picture = "SHP_HEALTHVAMPIRE", .lent = true},
}};

/** `picture`, when it is Dark Legacy's (`lent`) and `wanted` asks for Dark Legacy's, or it is
 * the N64's and `wanted` asks for the N64's; else none. */
std::optional<std::string_view> answer(std::string_view picture, bool lent, bool wanted) {
    return lent == wanted ? std::optional{picture} : std::nullopt;
}

TEST_CASE("the N64 draws nine of Dark Legacy's classes, and names a Pojo carrier Pojo",
          "[legends][n64-hud][n64-hud-icons]") {
    /** A class the N64 has portraits and a word for: e005a2f0's and e005a288's. */
    struct Native {
        s32 classIndex = 0;
        std::string_view bank;
        std::string_view code;
        s32 text = 0;
    };
    constexpr std::array kNative{
        Native{kWarriorClass, "hud/warf", "WAR", 129},
        Native{kValkyrieClass, "hud/valf", "VAL", 132},
        Native{kWizardClass, "hud/wizf", "WIZ", 130},
        Native{kArcherClass, "hud/arcf", "ARC", 131},
        Native{kMinotaurClass, "hud/minf", "MIN", 134},
        Native{kFalconessClass, "hud/falf", "FAL", 137},
        Native{kJackalClass, "hud/jacf", "JAC", 135},
        Native{kTigressClass, "hud/tigf", "TIG", 136},
        Native{kSumnerClass, "hud/sumf", "SUM", 138},
    };
    for (s32 classIndex = -1; classIndex <= kClassCount; ++classIndex) {
        CAPTURE(classIndex);
        const Native* native = nullptr;
        for (const Native& entry : kNative) {
            if (entry.classIndex == classIndex) {
                native = &entry;
            }
        }
        if (native == nullptr) {
            CHECK_FALSE(legendsN64ClassBank(classIndex).has_value());
            CHECK_FALSE(legendsN64ClassCode(classIndex).has_value());
            CHECK_FALSE(legendsN64ClassText(classIndex, false).has_value());
        } else {
            CHECK(legendsN64ClassBank(classIndex) == native->bank);
            CHECK(legendsN64ClassCode(classIndex) == native->code);
            CHECK(legendsN64ClassText(classIndex, false) == native->text);
            // Each portrait's code is the class's own asset code.
            CHECK(native->code == classCode(classIndex));
        }
        // Whatever the class, a character carrying Pojo is named by the N64's word for him.
        CHECK(legendsN64ClassText(classIndex, true) == 133);
    }
}

TEST_CASE("potion bottles follow Dark Legacy's kinds", "[legends][n64-hud][n64-hud-icons]") {
    // Dark Legacy's kinds colour its own icons red, red, blue, yellow and green; an empty stack
    // is kind 0.
    CHECK(StatusBoxPainter::potionIcon(1) == "POTION_ICON_RED");
    CHECK(StatusBoxPainter::potionIcon(2) == "POTION_ICON_BLU");
    CHECK(legendsN64PotionBottle(0) == "POTIONR");
    CHECK(legendsN64PotionBottle(1) == "POTIONR");
    CHECK(legendsN64PotionBottle(2) == "POTIONB");
    CHECK(legendsN64PotionBottle(3) == "POTIONY");
    CHECK(legendsN64PotionBottle(4) == "POTIONG");
    // A kind Dark Legacy does not know is shown red, as its own boxes show it.
    CHECK(StatusBoxPainter::potionIcon(5) == "POTION_ICON_RED");
    CHECK(legendsN64PotionBottle(5) == "POTIONR");
    CHECK(legendsN64PotionBottle(-1) == "POTIONR");
}

TEST_CASE("every Dark Legacy powerup pair has its N64 icon or its Dark Legacy picture",
          "[legends][n64-hud][n64-hud-icons]") {
    usize fallbacks = 0;
    for (const Pair& pair : kPairs) {
        CAPTURE(pair.kind, pair.flags);
        // The N64's icon or Dark Legacy's picture, never both.
        CHECK(legendsN64PowerupIcon(pair.kind, pair.flags) ==
              answer(pair.picture, pair.lent, false));
        CHECK(legendsN64PowerupFallback(pair.kind, pair.flags) ==
              answer(pair.picture, pair.lent, true));
        fallbacks += pair.lent ? 1 : 0;
    }
    CHECK(kPairs.size() - fallbacks == 31);
    CHECK(fallbacks == 7);
    // The named flags are the records' own.
    CHECK(damage::kThreeWayShot == 0x80000);
    CHECK(armor::kInvulnerable + armor::kGoldInvulnerable == 0x110000);
    CHECK((armor::kResistAcid | armor::kImmuneGas) == 0x2008);
    CHECK(powerup::kHealthVamp == 0x400000);

    // A magic powerup has no pair, and a powerup neither game knows shows nothing: no picture is
    // made up for it.
    for (const auto& [kind, flags] : std::array<std::pair<s32, u32>, 5>{{{powerup::kMagic, 0},
                                                                         {powerup::kMagic, 0x4},
                                                                         {powerup::kWeapon, 0},
                                                                         {powerup::kArmor, 0x1},
                                                                         {powerup::kSpecial, 0}}}) {
        CAPTURE(kind, flags);
        CHECK_FALSE(legendsN64PowerupIcon(kind, flags).has_value());
        CHECK_FALSE(legendsN64PowerupFallback(kind, flags).has_value());
    }

    // What the look takes as it opens: each icon and each picture once.
    std::vector<std::string_view> wanted;
    std::vector<std::string_view> lent;
    for (const Pair& pair : kPairs) {
        if (pair.lent) {
            lent.push_back(pair.picture);
        } else if (std::ranges::find(wanted, pair.picture) == wanted.end()) {
            wanted.push_back(pair.picture);
        }
    }
    for (const std::string_view icon :
         {"HUD_SCIMITAR", "HUD_ICEAXE", "HUD_SAVIOUR", "HUD_FLAMESCROLL", "HUD_JAVELIN"}) {
        wanted.push_back(icon);
    }
    std::vector<std::string_view> taken = legendsN64ItemIcons();
    std::ranges::sort(wanted);
    std::ranges::sort(taken);
    CHECK(taken == wanted);
    CHECK(taken.size() == 34);
    std::vector<std::string_view> borrowed = legendsN64PowerupFallbacks();
    std::ranges::sort(lent);
    std::ranges::sort(borrowed);
    CHECK(borrowed == lent);
}

TEST_CASE("both speed pairs show the speed boost and both gauntlets the right gauntlet",
          "[legends][n64-hud][n64-hud-icons]") {
    // Read backwards, the N64's items would miss Dark Legacy's speed powerup and its special
    // speed boost, which the N64 draws as one; it draws its left gauntlet (0xa50) with the right
    // one's icon.
    CHECK(legendsN64PowerupIcon(powerup::kSpeed, 0) == "HUD_SPEEDBOOST");
    CHECK(legendsN64PowerupIcon(powerup::kSpecial, powerup::kSpeedBoost) == "HUD_SPEEDBOOST");
    CHECK(legendsN64PowerupIcon(powerup::kSpecial, powerup::kRightGauntlet) == "HUD_SGAUNTR");
    CHECK(legendsN64PowerupIcon(powerup::kSpecial, powerup::kLeftGauntlet) == "HUD_SGAUNTR");
    // A speed powerup's flags change nothing: the kind has the one pair.
    CHECK(legendsN64PowerupIcon(powerup::kSpeed, 0x4) == "HUD_SPEEDBOOST");
}

TEST_CASE("gold invulnerability is matched before silver, and an amulet by its element",
          "[legends][n64-hud][n64-hud-icons]") {
    // Gold carries silver's flag too.
    CHECK(legendsN64PowerupIcon(powerup::kArmor, 0x110000) == "HUD_INVULNG");
    CHECK(legendsN64PowerupIcon(powerup::kArmor, armor::kGoldInvulnerable) == "HUD_INVULNG");
    CHECK(legendsN64PowerupIcon(powerup::kArmor, armor::kInvulnerable) == "HUD_INVULN");
    // An amulet's element is a number in the low nibble, not a flag: light (3) is not fire and
    // electricity (1 | 2).
    CHECK(legendsN64PowerupIcon(powerup::kWeapon, damage::kLight) == "HUD_LITEAMLT");
    CHECK(legendsN64PowerupIcon(powerup::kWeapon, damage::kAcid) == "HUD_ACIDAMLT");
    CHECK_FALSE(legendsN64PowerupIcon(powerup::kWeapon, 5).has_value());
    // A weapon's flags come before its element: a three-way shot of fire is the three-way shot.
    CHECK(legendsN64PowerupIcon(powerup::kWeapon, damage::kThreeWayShot | damage::kFire) ==
          "HUD_3WAY");
    // A pair carrying more than its flags is still that pair.
    CHECK(legendsN64PowerupIcon(powerup::kWeapon, damage::kHammer | damage::kKnockBack) ==
          "HUD_HAMMER");
    // Only a weapon has an element.
    CHECK_FALSE(legendsN64PowerupIcon(powerup::kSpecial, damage::kExplode).has_value());
}

TEST_CASE("the N64's legend icons cover five realms, and Dark Legacy's pictures the other four",
          "[legends][n64-hud][n64-hud-icons]") {
    // Dark Legacy's legend items by their bosses' realms, in order.
    CHECK(kLegendsN64LegendRealms == std::array<s32, 9>{1, 2, 3, 4, 5, 7, 9, 10, 11});
    struct Legend {
        s32 realm = 0;
        std::string_view picture;
        bool lent = false;
    };
    // game.bin's quest items (e00560a4) hold the scimitar, the ice axe, the saviour, the flame
    // scroll and the javelin; SELECT holds Dark Legacy's lamp, bellows, book and lantern.
    constexpr std::array<Legend, 9> kLegends{{
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
    for (const Legend& legend : kLegends) {
        CAPTURE(legend.realm);
        CHECK(legendsN64LegendIcon(legend.realm) == answer(legend.picture, legend.lent, false));
        CHECK(legendsN64LegendFallback(legend.realm) == answer(legend.picture, legend.lent, true));
    }
    // No other realm has a legend item.
    for (const s32 realm : {0, 6, 8, 12, 15}) {
        CAPTURE(realm);
        CHECK_FALSE(legendsN64LegendIcon(realm).has_value());
        CHECK_FALSE(legendsN64LegendFallback(realm).has_value());
    }
}

} // namespace
