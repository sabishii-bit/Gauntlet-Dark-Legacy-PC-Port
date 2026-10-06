#pragma once

#include "engine/assets/AnimationSet.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/ui/TextPainter.h"

#include "game/menu/BurnDialogueScroll.h"
#include "game/menu/SettingsMenu.h"
#include "game/players/Party.h"
#include "game/screens/GameContext.h"

namespace gdl::game {
enum class PauseOutcome : u8 { Running, Resume, Manage, Title, ReturnTower, Shop, Inventory };

/** A paused party's menu. File operations never mutate a running scene directly. */
class PauseMenu {
public:
    PauseMenu() = default;
    ~PauseMenu();
    GDL_NON_COPYABLE_NON_MOVABLE(PauseMenu);
    bool open(RenderDevice& device, const GameContext& context, std::span<const PartyMember> party,
              s32 player);
    void close();
    bool isOpen() const { return m_open; }
    s32 player() const { return m_player; }
    const std::vector<PartyMember>& party() const { return m_party; }
    PauseOutcome update(f64 seconds, const MenuInput& input);
    /** Whether the music plays on under the menu: only on the Audio page, so that its
     * slider can be heard (options.c 984); every other page ducks it (813). */
    bool musicAudible() const;
    /** Upload the dismissal mask after beginFrame, before the background's first draw. */
    void prepare(RenderDevice& device);
    void render(RenderDevice& device, const Mat4& projection, f32 width, f32 height);
    const OptionMenu& menu() const { return m_menu; }
    bool arrowBound() const { return m_arrow.bound(); }

private:
    enum class Page : u8 { Main, Options, Quit };
    void showMain();
    void showQuit();
    void loadDecorations(RenderDevice& device);
    void loadFireFrames(RenderDevice& device);
    PauseOutcome dismiss(PauseOutcome outcome);
    void playSound(std::string_view name);
    void stopSounds();
    void playMenuSound(const MenuEvent& event, bool horizontal);
    MenuDefinition backdrop() const;
    std::string text(std::string_view id) const;
    GameContext m_context;
    TextureSet m_textures;
    TextureSet m_powerupTextures;
    ModelSet m_powerupModels;
    AnimationSet m_powerupTrees;
    ModelSprite m_arrow;
    SoundSet m_commonSounds;
    std::vector<SoundHandle> m_soundHandles;
    BitmapFont m_font;
    TextPainter m_text;
    MenuTextures m_art;
    BurnDialogueScroll m_fire;
    std::vector<const Image*> m_fireMasks;
    std::vector<const Texture*> m_fireRing;
    const Image* m_scrollImage = nullptr;
    RenderDevice* m_device = nullptr;
    MenuScreen m_screen;
    Canvas m_canvas;
    Mat4 m_pointerTransform{1};
    OptionMenu m_menu;
    SettingsMenu m_settings;
    std::vector<PartyMember> m_party;
    Page m_page = Page::Main;
    PauseOutcome m_pendingOutcome = PauseOutcome::Running;
    s32 m_player = 0;
    f64 m_tickRemainder = 0.0;
    bool m_open = false;
    bool m_inTower = true;
    bool m_inSecretWorld = false;
};
} // namespace gdl::game
