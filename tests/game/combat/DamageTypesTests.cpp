#include <array>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/enemies/CritterData.h"
#include "game/players/ClassData.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("a damage type carries its element, its healing and whether it marks",
          "[game][damage-types]") {
    CHECK(damage::element(0) == 0);
    CHECK(damage::element(0x101) == damage::kFire);
    CHECK(damage::element(0x1000104) == damage::kAcid);
    CHECK(damage::element(0x20) == 0);
    CHECK_FALSE(damage::heals(0x200 | 1));
    CHECK(damage::heals(damage::kHeal | 0x200));
    CHECK(damage::marks(0x101));
    CHECK_FALSE(damage::marks(damage::kNoHitEffect | 0x100));
    // A caster's magic carries DMG_HEAL from level 25 (start_magic).
    CHECK(damage::magicHeal(1) == 0);
    CHECK(damage::magicHeal(24) == 0);
    CHECK(damage::magicHeal(25) == damage::kHeal);
    CHECK(damage::magicHeal(99) == damage::kHeal);
}

TEST_CASE("the common hit marks are blood or the element's burst, and the deaths their own",
          "[game][damage-types]") {
    CHECK(damage::hitEffect(0, false) == "BLOODFX1");
    CHECK(damage::hitEffect(0, true) == "BLOODFX2");
    CHECK(damage::hitEffect(damage::kFire, false) == "FIREHIT");
    CHECK(damage::hitEffect(damage::kFire, true) == "FIREDIE");
    CHECK(damage::hitEffect(damage::kElectric, false) == "HITCOL");
    CHECK(damage::hitEffect(damage::kElectric, true) == "ELECDIE");
    CHECK(damage::hitEffect(damage::kLight, false) == "HITCOL");
    CHECK(damage::hitEffect(damage::kLight, true) == "LIGHTDIE");
    CHECK(damage::hitEffect(damage::kAcid, false) == "HITCOL");
    CHECK(damage::hitEffect(damage::kAcid, true) == "ACIDDIE");
    CHECK(damage::hitEffect(5, false).empty());
    CHECK(damage::hitEffect(15, true).empty());
}

TEST_CASE("the elements are named by the player colours they belong to", "[game][damage-types]") {
    CHECK(damage::colourOf(0).empty());
    CHECK(damage::colourOf(damage::kFire) == "RED");
    CHECK(damage::colourOf(damage::kElectric) == "BLU");
    CHECK(damage::colourOf(damage::kLight) == "YEL");
    CHECK(damage::colourOf(damage::kAcid) == "GRE");
    CHECK(damage::colourOf(7).empty());
    // DamageColor (combat.c): fire is the third player's, lightning the second's, light the
    // first's and acid the fourth's; the colour codes run yellow, blue, red, green.
    for (s32 color = 0; color < 4; ++color) {
        CAPTURE(color);
        const u32 element = damage::elementOfColour(color);
        REQUIRE(element != 0);
        CHECK(damage::colourOf(element) == colorCode(color));
    }
    CHECK(damage::elementOfColour(-1) == 0);
    CHECK(damage::elementOfColour(4) == 0);
    // A potion of the caster's own colour is a tenth stronger (start_magic).
    CHECK(damage::colourBonus(2, damage::kFire) == Approx(1.1f));
    CHECK(damage::colourBonus(0, damage::kLight) == Approx(1.1f));
    CHECK(damage::colourBonus(0, damage::kFire) == 1.0f);
    CHECK(damage::colourBonus(2, 0) == 1.0f);
    CHECK(damage::colourBonus(-1, damage::kFire) == 1.0f);
}

TEST_CASE("the great ones' affinities come from their data and meet the element rules",
          "[game][damage-types][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DRAGON.json").parent_path();
    // The dragon shrugs off fire and fears lightning; the lich is acid's and fears light; the
    // golems take half of magic; Skorne's temple form is beyond magic altogether.
    struct Case {
        const char* name;
        u32 shield;
        u32 hitFlags;
        f32 share; ///< of a hit of twenty less armour, in a boss's arena
    };
    constexpr std::array kCases{
        Case{"DRAGON", 1, damage::kFire, 0.75f},     Case{"DRAGON", 1, damage::kElectric, 1.5f},
        Case{"DRAGON", 1, damage::kAcid, 1.25f},     Case{"LICH", 8, damage::kAcid, 0.75f},
        Case{"LICH", 8, damage::kLight, 1.5f},       Case{"YETI", 2, damage::kElectric, 0.75f},
        Case{"GOLEM", 16, Damage::kMagic, 0.75f},    Case{"GOLEM", 16, 0, 1.0f},
        Case{"SKORNE1", 4096, Damage::kMagic, 0.0f}, Case{"SKORNE1", 4096, damage::kFire, 1.25f},
    };
    for (const Case& c : kCases) {
        CAPTURE(c.name, c.hitFlags);
        CritterData data;
        REQUIRE(data.load(root / (std::string{c.name} + ".json")));
        REQUIRE(data.shieldFlags() == c.shield);
        const f32 plain = std::max(20.0f - data.armor(), 0.0f);
        const Damage modified =
            Damage::modify(20.0f, c.hitFlags, data.shieldFlags(), data.armor(), true);
        const f32 expected = (c.hitFlags & Damage::kMagic) != 0 ? 20.0f * c.share : plain * c.share;
        CHECK(modified.amount == Approx(expected));
    }
}

TEST_CASE("the classes' strike rows carry only the four elements",
          "[game][damage-types][unpacked]") {
    const auto root = test::unpackedOrSkip("pdata/WAR.json").parent_path();
    ClassDataSet classes;
    REQUIRE(classes.load(root));
    usize elemental = 0;
    for (s32 classIndex = 0; classIndex < kClassCount; ++classIndex) {
        const ClassStats* stats = classes.stats(classIndex);
        if (stats == nullptr) {
            continue;
        }
        for (const MoveStrike& strike : stats->moveStrikes) {
            CHECK(damage::element(strike.damageType) < damage::kElementCount);
            elemental += damage::element(strike.damageType) != 0 ? 1 : 0;
        }
    }
    CHECK(elemental > 0);
}
} // namespace
