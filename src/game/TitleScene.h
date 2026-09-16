#pragma once

#include <array>
#include <filesystem>
#include <string_view>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/ModelSprite.h"
#include "engine/ui/TextPainter.h"

#include "game/MenuInput.h"
#include "game/OptionMenu.h"

namespace gdl::game {

enum class TitleOutcome : u8 { Running, TimedOut, StartGame };

/**
 * The title screen: the logo backdrop with its animated glow, "Press Start", and the Start /
 * Options menus. Runs on the original's 60 Hz tick clock.
 */
class TitleScene {
public:
    static constexpr f32 kVirtualWidth = 512.0f;
    static constexpr f32 kVirtualHeight = 384.0f;
    static constexpr s32 kTicksPerSecond = 60;
    static constexpr s32 kIdleTicks = 1800;
    static constexpr s32 kGlowFadeInTicks = 60;
    static constexpr s32 kIdleFadeTicks = 30;
    static constexpr s32 kLoadingTicks = 30;
    static constexpr s32 kGlowFrames = 10;
    static constexpr s32 kFont32SpaceWidth = 16;

    /** Loads the unpacked title and shared textures; false (with a warning) when absent. Sounds
     * are optional: pass nullptr to run silently. */
    bool open(RenderDevice& device, SoundPlayer* sounds, const std::filesystem::path& unpackedRoot);
    void close();
    bool isOpen() const { return m_open; }

    TitleOutcome update(f64 deltaSeconds, const MenuInput& input);

    /** Steps the scene by whole ticks; update() calls this from wall-clock time. */
    TitleOutcome step(s32 ticks, const MenuInput& input);

    void render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth, f32 frameHeight);

    bool menuOpen() const { return m_titleMenu.isOpen(); }
    bool optionsOpen() const { return m_optionsMenu.isOpen(); }
    bool loading() const { return m_loadingTimer > 0; }
    s32 idleTicks() const { return m_idle; }
    s32 time() const { return m_time; }
    u8 glowOpacity() const { return m_glowOpacity; }
    bool musicPlaying() const;
    bool arrowBound() const { return m_arrow.bound(); }

private:
    bool loadResources(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    const Texture* staticTexture(std::string_view name, u32 frame = 0);
    void drawGlowText(s32 x, s32 y, std::string_view text);
    void openTitleMenu();
    void openOptionsMenu();
    void loadSounds(const std::filesystem::path& unpackedRoot);
    void loadArrow(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void startMusic();
    void playMenuSound(std::string_view name);

    bool m_open = false;
    RenderDevice* m_device = nullptr;
    SoundPlayer* m_sounds = nullptr;
    SoundSet m_commonSounds;
    SoundSet m_selectSounds;
    SoundHandle m_music = kNoSound;
    TextureSet m_titleTextures;
    TextureSet m_staticTextures;
    TextureSet m_powerupTextures;
    ModelSet m_powerupModels;
    AnimationSet m_powerupTrees;
    ModelSprite m_arrow;
    BitmapFont m_font32;
    TextPainter m_text;
    Canvas m_canvas;
    MenuTextures m_menuTextures;
    OptionMenu m_titleMenu;
    OptionMenu m_optionsMenu;
    std::array<u32, 4> m_backdrops{};
    u32 m_glowBase = 0;
    f64 m_tickRemainder = 0.0;
    s32 m_time = 0;
    s32 m_idle = kIdleTicks;
    s32 m_loadingTimer = 0;
    u8 m_glowOpacity = 0;
    bool m_glowHidden = false;
};

} // namespace gdl::game
