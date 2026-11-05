#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Clock.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/WorldCamera.h"

#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Party.h"
#include "game/config/ControlProfiles.h"
#include "game/context/GameServices.h"
#include "game/hud/StatusBox.h"
#include "game/level/LevelWorld.h"
#include "game/scenes/SelectLane.h"
#include "game/widgets/MenuInput.h"

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
    static_assert(static_cast<usize>(kLaneCount) == kInputSlots, "lane n reads input slot n");
    using Inputs = MenuInputs;

    PlayerSelectScene() = default;
    ~PlayerSelectScene();
    PlayerSelectScene(const PlayerSelectScene&) = delete;
    PlayerSelectScene& operator=(const PlayerSelectScene&) = delete;

    /** Loads the unpacked select assets; `startingPlayer` joins at once, and the characters
     * of `party` (a game in progress) stand locked in their lanes. False when absent. The
     * lanes stand in `tower`, loaded as the tower when it holds another level; without it
     * there is nothing behind them. The screen keeps reading `services`, which must outlive
     * it. */
    bool open(RenderDevice& device, const GameServices& services, s32 startingPlayer,
              std::span<const PartyMember> party = {}, bool manage = false,
              SoundHandle continuingMusic = kNoSound, LevelWorld* tower = nullptr);
    /** After a level, every surviving character gets Save/Change/Load/Quit/Done. */
    bool openAfterLevel(RenderDevice& device, const GameServices& services,
                        std::span<const PartyMember> party, LevelWorld* tower = nullptr);
    /** The screen keeps the services it is opened with, so they may not be a temporary. */
    bool open(RenderDevice&, const GameServices&&, s32, std::span<const PartyMember> = {},
              bool = false, SoundHandle = kNoSound, LevelWorld* = nullptr) = delete;
    bool openAfterLevel(RenderDevice&, const GameServices&&, std::span<const PartyMember>,
                        LevelWorld* = nullptr) = delete;
    /** Current characters, including those still choosing; dropped lanes are omitted. */
    std::vector<PartyMember> party() const;
    void close();
    bool isOpen() const { return m_open; }

    /** Applies all joining Start presses together without advancing existing lanes. */
    void join(const Inputs& inputs);

    SelectOutcome update(f64 deltaSeconds, const Inputs& inputs);

    /** Steps the screen by whole ticks; update() calls this from wall-clock time. */
    SelectOutcome step(s32 ticks, const Inputs& inputs);

    void render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth, f32 frameHeight);

    const SelectLane& lane(s32 index) const { return m_lanes[static_cast<usize>(index)]; }
    /** Whether any lane is taking a name, so the keyboard's escape belongs to it. */
    bool typing() const {
        return std::ranges::any_of(m_lanes, [](const SelectLane& lane) { return lane.typing(); });
    }

    /** Read assigned controllers, routing the keyboard to just the focused name field
     * while editing, even when that player normally uses a controller. */
    Inputs readInputs(const Input& input) const;
    s32 time() const { return m_time; }
    bool musicPlaying() const;

    /** Whether Sumner is still greeting a locked-in character. */
    bool speaking() const;
    const SaveSlots& saves() const { return m_saves; }
    bool towerVisible() const {
        return m_tower != nullptr && m_tower->built() && m_camera.has_value();
    }

private:
    /** The lane whose name field has the keyboard: the keyboard player's while it types,
     * else the first lane typing; -1 while none is. */
    s32 typist() const;
    bool loadResources(RenderDevice& device, const std::filesystem::path& unpackedRoot);
    void loadSounds(const std::filesystem::path& unpackedRoot);
    void loadTower(RenderDevice& device, LevelWorld* tower);
    void drawStatusBox(const SelectLane& lane);
    std::string_view text(std::string_view id) const;
    const Texture* selectTexture(std::string_view name);
    const Texture* staticTexture(std::string_view name);
    void playSound(SelectSound sound, const SelectLane& lane);
    void greetCharacter(SoundHandle greeting, const CharacterSave& save);
    void startMusic();
    void stopSounds();

    bool m_open = false;
    RenderDevice* m_device = nullptr;
    const GameServices* m_gameServices = &GameServices::none(); ///< the game's, from open on
    MenuScreen m_screen;
    s32 m_tickRate = 60;
    TextureSet m_selectTextures;
    TextureSet m_staticTextures;
    BitmapFont m_font32;
    BitmapFont m_font8;
    BitmapFont m_fontInitials;
    TextPainter m_large;
    TextPainter m_small;
    TextPainter m_initials;
    StatusBoxPainter m_boxes;
    Canvas m_canvas;
    Mat4 m_pointerTransform{1};
    SoundSet m_commonSounds;
    SoundSet m_selectSounds;
    SoundSet m_narratorSounds;
    SoundHandle m_music = kNoSound;
    SoundHandle m_greeting = kNoSound;
    std::vector<SoundHandle> m_soundHandles;
    ClassDataSet m_classes;
    LevelWorld* m_tower = nullptr;
    std::optional<WorldCamera> m_camera;
    SaveSlots m_saves;
    LaneServices m_services;
    std::array<SelectLane, kLaneCount> m_lanes{};
    std::vector<PartyMember> m_entryParty;
    static constexpr TickAccumulator::Limits kTicks{.mostTicks = 6}; ///< an update runs six at most
    TickAccumulator m_ticks{kTicks};
    s32 m_time = 0;
    s32 m_idleFrames = 0;
};

} // namespace gdl::game
