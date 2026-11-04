#include "game/config/BindingNames.h"

#include <string>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"

namespace gdl::game {

void readKeyNames(const nlohmann::json& list, std::string_view what, UnknownBinding policy,
                  bool keepsEscape, std::vector<Key>& keys) {
    keys.clear();
    for (const nlohmann::json& name : list) {
        const auto text = name.get<std::string>();
        const auto key = keyFromName(text);
        if (key.has_value()) {
            if (*key != Key::Escape || keepsEscape) {
                keys.push_back(*key);
            }
            continue;
        }
        switch (policy) {
        case UnknownBinding::WarnAndSkip:
            log::warn("Config: unknown key '{}' for '{}'", text, what);
            break;
        case UnknownBinding::Refuse: throw FormatError("unknown control key");
        }
    }
}

void readButtonNames(const nlohmann::json& list, std::string_view what, UnknownBinding policy,
                     std::vector<PadButton>& buttons) {
    buttons.clear();
    for (const nlohmann::json& name : list) {
        const auto text = name.get<std::string>();
        const auto button = padButtonFromName(text);
        if (button.has_value()) {
            buttons.push_back(*button);
            continue;
        }
        switch (policy) {
        case UnknownBinding::WarnAndSkip:
            log::warn("Config: unknown pad button '{}' for '{}'", text, what);
            break;
        case UnknownBinding::Refuse: throw FormatError("unknown control button");
        }
    }
}

} // namespace gdl::game
