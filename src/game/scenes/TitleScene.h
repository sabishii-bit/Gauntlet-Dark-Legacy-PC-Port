#pragma once

#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Clock.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/ModelSprite.h"
#include "engine/ui/TextPainter.h"

#include "game/context/GameServices.h"
#include "game/menu/LiveSettings.h"
#include "game/menu/SettingsMenu.h"
#include "game/widgets/BurnDialogueScroll.h"
#include "game/widgets/MenuInput.h"
#include "game/widgets/OptionMenu.h"

namespace gdl::game {

enum class TitleOutcome : u8 { Running, TimedOut, StartGame, Netplay, Quit };

/**
 * The title screen: the logo backdrop with its animated glow, "Press Start", and the Start /
 * Options menus. Runs on the configured tick clock.
 */
class TitleScene {
public:
    static constexpr s32 kIdleTicks = 1800;
    static constexpr s32 kGlowFadeInTicks = 60;
    static constexpr s32 kIdleFadeTicks = 30;
    static constexpr s32 kLoadingTicks = 30;
    static constexpr s32 kGlowFrames = 10;
    static constexpr s32 kFont32SpaceWidth = 16;

    /** Loads the unpacked title and shared assets; false (with a warning) when absent.
     * `settings` saves and previews what Options changes; without it every change is refused.
     * The screen keeps reading `services`, which must outlive it. */
    bool open(RenderDevice& device, const GameServices& services, LiveSettings* settings = nullptr);
    /** The screen keeps the services it is opened with, so they may not be a temporary. */
    bool open(RenderDevice&, const GameServices&&, LiveSettings* = nullptr) = delete;
    void close();
    bool isOpen() const { return m_open; }

    TitleOutcome update(f64 deltaSeconds, const MenuInput& input);

    /** Steps the scene by whole ticks; update() calls this from wall-clock time. */
    TitleOutcome step(s32 ticks, const MenuInput& input);
    const SettingsMenu& settings() const { return m_optionsMenu; }

    void render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth, f32 frameHeight);

    bool menuOpen() const { return m_titleMenu.isOpen(); }
    const OptionMenu& titleMenu() const { return m_titleMenu; }
    bool startMenuOpen() const { return menuOpen() && m_startMenu; }
    /** Whether the menu is asking before quitting to the desktop. */
    bool quitConfirmOpen() const { return menuOpen() && m_quitConfirm; }
    bool optionsOpen() const { return m_optionsMenu.isOpen(); }
    bool loading() const { return m_loadingTimer > 0; }
    bool burning() const { return m_fire.active(); }
    s32 idleTicks() const { return m_idle; }
    s32 time() const { return m_time; }
    s32 tickRate() const { return m_tickRate; }
    const MenuScreen& screen() const { return m_screen; }
    u8 glowOpacity() const { return m_glowOpacity; }
    bool musicPlaying() const;
    SoundHandle musicHandle() const { return m_music; }
    /** Relinquish a track successfully adopted by the character-select screen. */
    void releaseMusic() { m_music = kNoSound; }
    bool arrowBound() const { return m_arrow.bound(); }

private:
    bool loadResources(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void loadSounds(const std::filesystem::path& unpackedRoot);
    void loadArrow(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void loadFireFrames(RenderDevice& device);
    const Texture* staticTexture(std::string_view name, u32 frame = 0);
    void drawGlowText(s32 x, s32 y, std::string_view label);
    void openTitleMenu(bool startMenu = false);
    void openQuitConfirm();
    void openOptionsMenu();
    void closeOptionsMenu();
    void startMusic();
    void playMenuSound(std::string_view name);
    std::string_view text(std::string_view id) const;

    bool m_open = false;
    bool m_startMenu = false;
    bool m_quitConfirm = false;    ///< the title menu is the quit dialog
    std::string m_optionsLanguage; ///< the text language when the options opened
    RenderDevice* m_device = nullptr;
    const GameServices* m_services = &GameServices::none(); ///< the game's, from open on
    LiveSettings* m_liveSettings = nullptr;
    MenuScreen m_screen;
    s32 m_tickRate = 60;
    TextureSet m_titleTextures;
    TextureSet m_staticTextures;
    TextureSet m_powerupTextures;
    ModelSet m_powerupModels;
    AnimationSet m_powerupTrees;
    ModelSprite m_arrow;
    SoundSet m_commonSounds;
    SoundSet m_selectSounds;
    SoundHandle m_music = kNoSound;
    BitmapFont m_font32;
    TextPainter m_text;
    Canvas m_canvas;
    Mat4 m_pointerTransform{1};
    MenuTextures m_menuTextures;
    OptionMenu m_titleMenu;
    SettingsMenu m_optionsMenu;
    BurnDialogueScroll m_fire;
    std::vector<const Image*> m_fireMasks;
    std::vector<const Texture*> m_fireRing;
    const Image* m_scrollImage = nullptr;
    std::array<u32, 4> m_backdrops{};
    u32 m_glowBase = 0;
    static constexpr TickAccumulator::Limits kTicks{.mostTicks = 6}; ///< an update runs six at most
    TickAccumulator m_ticks{kTicks};
    s32 m_time = 0;
    s32 m_idle = kIdleTicks;
    s32 m_loadingTimer = 0;
    u8 m_glowOpacity = 0;
    bool m_glowHidden = false;
};

} // namespace gdl::game
