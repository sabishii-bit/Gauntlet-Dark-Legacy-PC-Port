#include "game/players/PickupVoices.h"

#include <format>

#include "engine/core/Types.h"

#include "game/players/ClassData.h"

namespace gdl::game {

PickupVoice PickupVoices::food(s32 character, std::string_view name, bool poisoned, bool pojo) {
    const bool spoken = !poisoned && std::uniform_int_distribution<s32>(0, 3)(m_random) == 0;
    return foodChoice(character, name, poisoned, pojo, spoken);
}

PickupVoice PickupVoices::foodChoice(s32 character, std::string_view name, bool poisoned, bool pojo,
                                     bool spoken) {
    if (pojo) {
        if (poisoned) {
            return {"S_POJOPOISON", true};
        }
        // Retail's one-in-four spoken branch is silent for Pojo, not another eating sound.
        return {spoken ? "" : "S_POJOEATSFX", true};
    }
    const s32 base = character % kStartingClassCount;
    const auto code = classCode(base);
    if (poisoned) {
        return {std::format("S_{}POISON", code)};
    }
    if (!spoken) {
        return {std::format("S_{}EATSFX", code)};
    }
    if (base == 3) {
        // items.c's descriptor comparisons: default meat, APPLE, BANANA, PINEAPPLE.
        s32 food = 1;
        if (name == "APPLE") {
            food = 2;
        } else if (name == "BANANA") {
            food = 3;
        } else if (name == "PINEAPPLE") {
            food = 4;
        }
        return {std::format("S_ARCEAT{}", food)};
    }
    return {std::format("S_{}EAT", code)};
}

std::string PickupVoices::bonusGold(s32 player, s32 amount) {
    std::string_view metal = "GOLD";
    if (amount == 50) {
        metal = "BRONZE";
    } else if (amount == 100) {
        metal = "SILVER";
    }
    return std::format("S_PKUP{}{}", metal, player + 1);
}

} // namespace gdl::game
