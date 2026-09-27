#pragma once

#include <random>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

#include "game/players/CharacterSave.h"

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
    /** The narrator's name for a character ("S_REDMIN2", red minotaur), found in its shadow
     * class's bank: every one of the sixteen has its own (AudioWithName); Sumner, who has
     * none, takes the warrior's. */
    static std::string nameOf(s32 character, s32 color);
    /** Whether Pojo speaks for the character: it carries him switched on. */
    static bool carriesPojo(const CharacterSave& save);
    /** fn_8009CFA8's per-player secret-realm coin sounds. */
    static std::string bonusGold(s32 player, s32 amount);

private:
    std::mt19937 m_random{0xF00Du};
};

} // namespace gdl::game
