#pragma once

#include <functional>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"

#include "game/config/GameConfig.h"
#include "game/menu/OptionMenu.h"

namespace gdl::game {

/** Shared title/pause settings with transactional edits; Controls is not implemented. */
class SettingsMenu {
public:
    enum class Page : u8 { Root, Audio, Game, Compass, Difficulty };
    enum class Scope : u8 { Title, Tower, Level };
    using Persist = std::function<bool(const GameConfig&)>;
    using PreviewAudio = std::function<void(const AudioConfig&)>;
    void open(const GameConfig& config, const StringTable* strings, Persist persist,
              const TextPainter& painter, const MenuScreen& screen, MenuDefinition backdrop,
              Scope scope = Scope::Title, PreviewAudio preview = {});
    MenuEvent update(const MenuInput& input, s32 ticks);
    void close() { m_menu.close(); }
    bool isOpen() const { return m_menu.isOpen(); }
    Page page() const { return m_page; }
    const GameConfig& config() const { return m_config; }
    const OptionMenu& menu() const { return m_menu; }
    OptionMenu& menu() { return m_menu; }
    void draw(Canvas& canvas, const TextPainter& painter, const MenuTextures& textures) const;

private:
    void rebuild(s32 selection = 0);
    void change(s32 direction);
    bool flushAudio();
    void commit(GameConfig next);
    std::string text(std::string_view id) const;
    OptionMenu m_menu;
    MenuDefinition m_backdrop;
    MenuScreen m_screen;
    const TextPainter* m_painter = nullptr;
    const StringTable* m_strings = nullptr;
    Persist m_persist;
    PreviewAudio m_previewAudio;
    bool m_audioDirty = false;
    Scope m_scope = Scope::Title;
    GameConfig m_config;
    Page m_page = Page::Root;
    std::string m_notice;
};
} // namespace gdl::game
