#pragma once
#include <functional>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

#include "game/hud/ControlPrompts.h"

namespace gdl::test {
/** Control names a test gives through `answer`; without one, every name is empty. */
class ScriptedControlLabels final : public game::ControlLabels {
public:
    std::string label(s32 player, std::string_view action) const override {
        return answer ? answer(player, action) : std::string{};
    }

    std::function<std::string(s32, std::string_view)> answer;
};
} // namespace gdl::test
