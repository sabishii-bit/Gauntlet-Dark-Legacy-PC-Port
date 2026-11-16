#include "expansions/n64/hud/LegendsN64HudIcons.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

#include "engine/core/Types.h"

#include "game/characters/ClassData.h"
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

} // namespace gdl::game
