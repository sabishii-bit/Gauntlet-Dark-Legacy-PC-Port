#include "game/players/MagicPerks.h"

#include "game/players/ClassData.h"

namespace gdl::game {
namespace {
constexpr s32 kFamilies = 4; ///< the class table repeats warrior, valkyrie, wizard, archer
} // namespace

std::optional<MagicPerk> MagicPerk::of(s32 character, s32 level) {
    if (level < kLevel || character < 0 || character >= kClassCount) {
        return std::nullopt;
    }
    // change_player puts Sumner among the wizards.
    const auto family = character == kSumnerClass
                            ? MagicPerkFamily::Food
                            : static_cast<MagicPerkFamily>(character % kFamilies);
    return MagicPerk{.family = family, .greater = level >= kGreaterLevel};
}

} // namespace gdl::game
