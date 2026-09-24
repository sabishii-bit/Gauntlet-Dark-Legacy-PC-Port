#pragma once

#include <random>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/** AudioPlayerEatFood / AudioPlayerSeverePain's class or Pojo response. Empty is intentional. */
struct PickupVoice {
    std::string sound;
    bool common = false;
};

/** Owns food-response variation independently of rendering and inventory mutation. */
class PickupVoices {
public:
    PickupVoice food(s32 character, std::string_view name, bool poisoned, bool pojo);
    /** The special spoken response is selected on one of four rolls, otherwise eating SFX. */
    static PickupVoice foodChoice(s32 character, std::string_view name, bool poisoned, bool pojo,
                                  bool spoken);
    /** fn_8009CFA8's per-player secret-realm coin sounds. */
    static std::string bonusGold(s32 player, s32 amount);

private:
    std::mt19937 m_random{0xF00Du};
};

} // namespace gdl::game
