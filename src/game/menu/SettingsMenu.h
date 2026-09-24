#pragma once

#include <functional>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/platform/Input.h"

#include "game/config/GameConfig.h"
#include "game/menu/OptionMenu.h"

namespace gdl::game {

/** Shared title/pause settings, with transactional edits and gameplay-binding capture. */
class SettingsMenu {
public:
    enum class Page : u8 { Root, Audio, Game, Compass, Controls };
    using Persist = std::function<bool(const GameConfig&)>;
    void open(const GameConfig& config, const StringTable* strings, Persist persist,
              const TextPainter& painter, const MenuScreen& screen, MenuDefinition backdrop);
    MenuEvent update(const MenuInput& input, s32 ticks, const Input* raw = nullptr, s32 pad = -1);
    void close() {
        m_menu.close();
        m_capturing = false;
    }
    bool isOpen() const { return m_menu.isOpen(); }
    bool capturing() const { return m_capturing; }
    Page page() const { return m_page; }
    const GameConfig& config() const { return m_config; }
    const OptionMenu& menu() const { return m_menu; }
    OptionMenu& menu() { return m_menu; }
    void draw(Canvas& canvas, const TextPainter& painter, const MenuTextures& textures) const;

private:
    void rebuild(s32 selection = 0);
    void change(s32 direction);
    void capture(const Input& raw, s32 pad);
    void commit(GameConfig next);
    std::string text(std::string_view id) const;
    std::string bindingLabel() const;
    OptionMenu m_menu;
    MenuDefinition m_backdrop;
    MenuScreen m_screen;
    const TextPainter* m_painter = nullptr;
    const StringTable* m_strings = nullptr;
    Persist m_persist;
    GameConfig m_config;
    Page m_page = Page::Root;
    bool m_pad = false;
    bool m_capturing = false;
    usize m_action = 0;
    std::string m_notice;
};
} // namespace gdl::game
