#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/ModelSprite.h"
#include "engine/ui/TextPainter.h"

#include "game/menu/BurnDialogueScroll.h"
#include "game/menu/MenuInput.h"
#include "game/menu/OptionMenu.h"
#include "game/screens/GameContext.h"

namespace gdl::game {

enum class TitleOutcome : std::uint8_t { Running, TimedOut, StartGame };

/**
 * The title screen: the logo backdrop with its animated glow, "Press Start", and the Start /
 * Options menus. Runs on the configured tick clock.
 */
class TitleScene {
public:
    static constexpr std::int32_t kIdleTicks = 1800;
    static constexpr std::int32_t kGlowFadeInTicks = 60;
    static constexpr std::int32_t kIdleFadeTicks = 30;
    static constexpr std::int32_t kLoadingTicks = 30;
    static constexpr std::int32_t kGlowFrames = 10;
    static constexpr std::int32_t kFont32SpaceWidth = 16;

    /** Loads the unpacked title and shared assets; false (with a warning) when absent. */
    bool open(RenderDevice& device, const GameContext& context);
    void close();
    bool isOpen() const { return m_open; }

    TitleOutcome update(double deltaSeconds, const MenuInput& input);

    /** Steps the scene by whole ticks; update() calls this from wall-clock time. */
    TitleOutcome step(std::int32_t ticks, const MenuInput& input);

    void render(RenderDevice& device, const Mat4& frameProjection, float frameWidth,
                float frameHeight);

    bool menuOpen() const { return m_titleMenu.isOpen(); }
    bool optionsOpen() const { return m_optionsMenu.isOpen(); }
    bool loading() const { return m_loadingTimer > 0; }
    bool burning() const { return m_fire.active(); }
    std::int32_t idleTicks() const { return m_idle; }
    std::int32_t time() const { return m_time; }
    std::int32_t tickRate() const { return m_tickRate; }
    const MenuScreen& screen() const { return m_screen; }
    std::uint8_t glowOpacity() const { return m_glowOpacity; }
    bool musicPlaying() const;
    bool arrowBound() const { return m_arrow.bound(); }

private:
    bool loadResources(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void loadSounds(const std::filesystem::path& unpackedRoot);
    void loadArrow(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void loadFireFrames(RenderDevice& device);
    const Texture* staticTexture(std::string_view name, std::uint32_t frame = 0);
    void drawGlowText(std::int32_t x, std::int32_t y, std::string_view label);
    void openTitleMenu();
    void openOptionsMenu();
    void closeOptionsMenu();
    void startMusic();
    void playMenuSound(std::string_view name);
    std::string_view text(std::string_view id) const;

    bool m_open = false;
    RenderDevice* m_device = nullptr;
    GameContext m_context;
    MenuScreen m_screen;
    std::int32_t m_tickRate = 60;
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
    MenuTextures m_menuTextures;
    OptionMenu m_titleMenu;
    OptionMenu m_optionsMenu;
    BurnDialogueScroll m_fire;
    std::vector<const Image*> m_fireMasks;
    std::vector<const Texture*> m_fireRing;
    const Image* m_scrollImage = nullptr;
    std::array<std::uint32_t, 4> m_backdrops{};
    std::uint32_t m_glowBase = 0;
    double m_tickRemainder = 0.0;
    std::int32_t m_time = 0;
    std::int32_t m_idle = kIdleTicks;
    std::int32_t m_loadingTimer = 0;
    std::uint8_t m_glowOpacity = 0;
    bool m_glowHidden = false;
};

} // namespace gdl::game
