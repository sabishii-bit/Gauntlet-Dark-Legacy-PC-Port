#include <array>
#include <optional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "expansions/n64/hud/LegendsN64HudIcons.h"
#include "game/characters/ClassData.h"
#include "game/hud/StatusBox.h"

namespace {

using namespace gdl;
using namespace gdl::game;

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
    CHECK(legendsN64HudTextKey(129) == "legends.n64.hud.text.129");
    CHECK(legendsN64HudTextKey(138) == "legends.n64.hud.text.138");
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

} // namespace
