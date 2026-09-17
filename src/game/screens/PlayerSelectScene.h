#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldScene.h"

#include "game/menu/MenuInput.h"
#include "game/players/CharacterSave.h"
#include "game/players/ClassData.h"
#include "game/screens/GameContext.h"
#include "game/screens/SelectLane.h"

namespace gdl::game {

enum class SelectOutcome : u8 { Running, Cancelled, Done };

/**
 * The player select screen: four lanes, one per player, where each joins with Start and
 * creates or loads a character. Ends once every player has locked in, or when the last one
 * backs out.
 */
class PlayerSelectScene {
public:
    static constexpr s32 kLaneCount = 4;
    static constexpr s32 kIdleFrames = 8;
    using Inputs = std::array<MenuInput, kLaneCount>;

    /** Loads the unpacked select assets; `startingPlayer` joins at once. False when absent. */
    bool open(RenderDevice& device, const GameContext& context, s32 startingPlayer);
    void close();
    bool isOpen() const { return m_open; }

    SelectOutcome update(f64 deltaSeconds, const Inputs& inputs);

    /** Steps the screen by whole ticks; update() calls this from wall-clock time. */
    SelectOutcome step(s32 ticks, const Inputs& inputs);

    void render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth, f32 frameHeight);

    const SelectLane& lane(s32 index) const { return m_lanes[static_cast<usize>(index)]; }

    /** The devices lane `index` reads this frame: its player's, typing while it takes a
     * name. */
    MenuInputSource inputSource(s32 index) const;
    s32 time() const { return m_time; }
    bool musicPlaying() const;
    const SaveSlots& saves() const { return m_saves; }
    bool towerVisible() const { return m_tower.built() && m_camera.has_value(); }
    const WorldScene& tower() const { return m_tower; }

private:
    bool loadResources(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void loadSounds(const std::filesystem::path& unpackedRoot);
    void loadTower(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void drawStatusBox(const SelectLane& lane);
    std::string_view text(std::string_view id) const;
    const Texture* selectTexture(std::string_view name);
    const Texture* staticTexture(std::string_view name);
    void playSound(SelectSound sound);
    void startMusic();

    bool m_open = false;
    RenderDevice* m_device = nullptr;
    GameContext m_context;
    MenuScreen m_screen;
    s32 m_tickRate = 60;
    TextureSet m_selectTextures;
    TextureSet m_staticTextures;
    BitmapFont m_font32;
    BitmapFont m_font8;
    BitmapFont m_fontInitials;
    BitmapFont m_fontScore;
    BitmapFont m_fontSmallCaps;
    TextPainter m_large;
    TextPainter m_small;
    TextPainter m_initials;
    TextPainter m_score;
    TextPainter m_smallCaps;
    Canvas m_canvas;
    SoundSet m_commonSounds;
    SoundSet m_selectSounds;
    SoundHandle m_music = kNoSound;
    ClassDataSet m_classes;
    ModelSet m_towerModels;
    TextureSet m_towerTextures;
    WorldLayout m_towerLayout;
    WorldScene m_tower;
    std::optional<WorldCamera> m_camera;
    SaveSlots m_saves;
    LaneServices m_services;
    std::array<SelectLane, kLaneCount> m_lanes{};
    f64 m_tickRemainder = 0.0;
    s32 m_time = 0;
    s32 m_idleFrames = 0;
};

} // namespace gdl::game
