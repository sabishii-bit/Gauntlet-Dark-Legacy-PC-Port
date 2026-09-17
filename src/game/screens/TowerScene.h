#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/MessageTable.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"

#include "game/menu/MenuInput.h"
#include "game/menu/ScrollBox.h"
#include "game/players/CharacterSave.h"
#include "game/players/ClassData.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PlayerControls.h"
#include "game/screens/GameContext.h"
#include "game/screens/PickupHud.h"
#include "game/screens/StatusBox.h"
#include "game/world/SumnerFigure.h"
#include "game/world/TowerCamera.h"
#include "game/world/TowerWorld.h"

namespace gdl::game {

/** A locked-in character and the player who drives it. */
struct PartyMember {
    s32 player = 0;
    CharacterSave save;
};

/** How the tower may open other than as the game does, for tests and scenarios. */
struct TowerOptions {
    std::optional<Vec3> position; ///< where the party stands instead of the entrance
    std::optional<f32> yaw;       ///< the way it faces, instead of the entrance's
    std::optional<bool> welcome;  ///< whether Sumner welcomes it, else by its experience
};

/** One player's input for a frame of play. */
struct PlayInput {
    MoveInput move;
    MenuInput menu;
};

enum class TowerOutcome : u8 { Running, Leave };

/**
 * The tower with the party in it: each character stands at the entrance and walks under its
 * player's control, its body playing the class's sequences and its feet sounding on the stone,
 * the camera follows from the level's marker angles, the level's music loops from its stream,
 * Sumner idles at his lookout, and the status boxes line the bottom of the screen. A party new
 * to the tower is welcomed the way the original does it: his scroll of pages first, then his
 * gesture as the camera cuts to the crystals for five seconds with the party held still.
 */
class TowerScene {
public:
    static constexpr s32 kPlayerCount = 4;
    static constexpr f32 kSpawnSpacing = 2.0f; ///< between party members at the entrance
    static constexpr u32 kCrystalCamera = 198; ///< the trigger camera the welcome cuts to
    static constexpr s32 kCrystalTicks = 300;  ///< fifty frames of six ticks
    using Inputs = std::array<PlayInput, kPlayerCount>;

    /** Where a new party's welcome has got to. */
    enum class Intro : u8 { None, Scroll, Crystal, Done };

    /** Brings the party into `world` (loading it when needed); false when the level or the
     * status boxes are not unpacked. */
    bool open(RenderDevice& device, const GameContext& context, TowerWorld& world,
              std::span<const PartyMember> party, const TowerOptions& options = {});
    void close();
    bool isOpen() const { return m_open; }

    TowerOutcome update(f64 deltaSeconds, const Inputs& inputs);
    void render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                f32 frameHeight);

    usize actorCount() const { return m_actors.size(); }
    /** The character driven by `player`, or null when that player is not in the party. */
    const PlayerActor* actor(s32 player) const;
    /** The body animation of `player`'s character, or null without a figure for it. */
    const PlayerAnimator* animator(s32 player) const;
    const TowerCamera& camera() const { return m_camera; }
    /** The camera the scene is seen through: the crystals during the welcome's cut, else the
     * follow camera. */
    const WorldCamera& viewCamera() const;
    /** The music's voice, kNoSound while nothing plays. */
    SoundHandle music() const { return m_music; }
    Intro intro() const { return m_intro; }
    const ScrollBox& scroll() const { return m_scroll; }
    const PickupHud& pickups() const { return m_pickups; }
    const SumnerFigure& sumner() const { return m_sumner; }

private:
    /** A character's model as it stands in the tower, and the class's sequences it plays. */
    struct Figure {
        ModelSet models;
        TextureSet textures;
        AnimationSet trees;   ///< the costume's own tree
        AnimationSet actions; ///< the class's sequences
        TreeModel model;
        PlayerAnimator animator;
        const TreeInfo* costume = nullptr;
        std::vector<s32> classNodeOfNode; ///< per costume node: the class tree node it follows
        std::vector<Mat4> transforms;     ///< per costume node, from the current pose

        void animate(f32 stickMagnitude, s32 ticks, f32 seconds);
    };

    void spawnParty(std::span<const PartyMember> party, const TowerOptions& options);
    std::unique_ptr<Figure> loadFigure(RenderDevice& device, const CharacterSave& save);
    void loadActions(Figure& figure, const CharacterSave& save);
    void loadSounds();
    void startMusic();
    void playStep(PlayerAnimator::Foot foot);
    void loadIntroArt(RenderDevice& device);
    static bool freshParty(std::span<const PartyMember> party);
    void beginIntro(RenderDevice& device);
    void startCrystalCut();
    u32 acceptedPlayers(const Inputs& inputs) const;
    std::vector<TriggerVisitor> visitors() const;
    void collectItems();
    void playCommon(std::optional<u32> sound);
    StatusBoxView statusOf(s32 player) const;
    CameraView cameraView() const;

    bool m_open = false;
    RenderDevice* m_device = nullptr;
    GameContext m_context;
    TowerWorld* m_world = nullptr;
    ClassDataSet m_classes;
    StatusBoxPainter m_boxes;
    PickupHud m_pickups;
    Canvas m_canvas;
    TowerCamera m_camera;
    std::vector<PlayerActor> m_actors;
    std::vector<std::unique_ptr<Figure>> m_figures; ///< one per actor, null when unavailable
    std::vector<CameraSubject> m_subjects; ///< one per actor, refreshed every frame
    SoundSet m_commonSounds;
    std::array<std::optional<u32>, 2> m_stepSounds{};
    std::optional<u32> m_pickupSound; ///< one per foot
    SoundHandle m_music = kNoSound;
    SumnerFigure m_sumner;
    TextureSet m_staticTextures;
    BitmapFont m_font32;
    TextPainter m_text;
    MessageTable m_scrollText;
    ScrollBox m_scroll;
    Intro m_intro = Intro::None;
    WorldCamera m_cutCamera;
    s32 m_cutTicks = 0;
};

} // namespace gdl::game
