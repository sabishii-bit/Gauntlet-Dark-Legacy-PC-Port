#pragma once

#include "engine/assets/BitmapFont.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/ui/TextPainter.h"

#include "game/menu/SettingsMenu.h"
#include "game/screens/GameContext.h"
#include "game/screens/SaveMenu.h"

namespace gdl::game {
enum class PauseOutcome : u8 { Running, Resume, Reload, Title };

/** A paused party's menu. File operations never mutate a running scene directly. */
class PauseMenu {
public:
    PauseMenu() = default;
    ~PauseMenu() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(PauseMenu);
    bool open(RenderDevice& device, const GameContext& context, std::span<const PartyMember> party,
              s32 player);
    void close();
    bool isOpen() const { return m_open; }
    s32 player() const { return m_player; }
    const std::vector<PartyMember>& party() const { return m_party; }
    PauseOutcome update(f64 seconds, const MenuInput& input, const Input* raw = nullptr);
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height);
    const OptionMenu& menu() const { return m_menu; }

private:
    enum class Page : u8 { Main, Files, Options, Quit };
    void showMain();
    void showFiles();
    MenuDefinition backdrop() const;
    std::string text(std::string_view id) const;
    GameContext m_context;
    TextureSet m_textures;
    BitmapFont m_font;
    TextPainter m_text;
    MenuTextures m_art;
    MenuScreen m_screen;
    Canvas m_canvas;
    OptionMenu m_menu;
    SettingsMenu m_settings;
    SaveSlots m_slots;
    SaveMenu m_files;
    std::vector<PartyMember> m_party;
    Page m_page = Page::Main;
    s32 m_player = 0;
    bool m_open = false;
};
} // namespace gdl::game
