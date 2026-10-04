#pragma once
#include <functional>

#include "game/menu/OptionMenu.h"

namespace gdl::game {
/** Per-player editor. Captures only the selected device, after the opening press is released. */
class ControlSettings {
public:
    void begin(const GameConfig& config);
    void define(MenuDefinition& definition, const TextPainter& painter,
                const std::function<std::string(std::string_view)>& text) const;
    /** Returns true when the controls root is closed. Edits are committed transactionally. */
    bool update(const MenuInput& input, s32 ticks, OptionMenu& menu, GameConfig& config,
                const std::function<bool(const GameConfig&)>& persist,
                const std::function<void(s32)>& rebuild);

private:
    std::vector<PlayerControlConfig> choices(const GameConfig& config) const;
    Input m_devices;
    GameConfig m_draft;
    GameConfig m_navigation;
    s32 m_player = -1;
    s32 m_page = 0;
    s32 m_capture = -1;
    s32 m_timeout = 0;
    bool m_released = false;
    bool m_failed = false;
};
} // namespace gdl::game
