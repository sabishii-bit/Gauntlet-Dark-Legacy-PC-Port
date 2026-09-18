#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/ItemArchive.h"
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
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreePose.h"

#include "game/world/AmbientSounds.h"
#include "game/world/StartCamera.h"
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
    std::optional<Vec3> position; ///< where the party stands instead of the entrance (the
                                  ///< start camera then does not ride in from its marker)
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
 * Sumner idles at his lookout, and the status boxes line the bottom of the screen. The party
 * materialises held still, playing its entrance, under the level's title while the start
 * camera holds at the entrance marker and rides in to the follow camera. A party new to the
 * tower is then welcomed the way the original does it: his scroll of pages first, then his
 * gesture as the camera cuts to the crystals for five seconds, between black bars, with the
 * party held still.
 */
class TowerScene {
public:
    static constexpr s32 kPlayerCount = 4;
    static constexpr f32 kSpawnSpacing = 2.0f; ///< between party members at the entrance
    static constexpr u32 kCrystalCamera = 198; ///< the trigger camera the welcome cuts to
    static constexpr f32 kBeamRadius = 12.0f;  ///< how near Sumner his beam of light comes on
    static constexpr s32 kSpawnTicks = 60;     ///< the materialising effect's life
    /** The voice that announces each realm's gate opening, by realm. */
    static constexpr std::array<std::string_view, 9> kUnlockVoices{
        "",           "S_CRYS4TWN", "S_CRYS4MNT", "S_CRYS4CST", "S_CRYS4SKY",
        "S_CRYS4FOR", "S_CRYS4DES", "S_CRYS4ICE", "S_CRYS4DRM"};
    static constexpr s32 kBeamFadeTicks = 180; ///< and how long it takes to come up or go
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
    /** The camera the scene is seen through: the start camera while it holds and rides in,
     * the crystals during the welcome's cut, else the follow camera. */
    const WorldCamera& viewCamera() const;
    const StartCamera& startCamera() const { return m_startCamera; }
    /** The music's voice, kNoSound while nothing plays. */
    SoundHandle music() const { return m_music; }
    /** Sumner's voice over the scroll he is reading, kNoSound while he is quiet. */
    SoundHandle voice() const { return m_voice; }
    /** The sound of the target that opened before the party most recently and is still
     * opening (a gate's force field humming as it thins, a lift, a gate), kNoSound otherwise. */
    SoundHandle fieldSound() const {
        return m_openingSounds.empty() ? kNoSound : m_openingSounds.back().handle;
    }
    Intro intro() const { return m_intro; }
    const ScrollBox& scroll() const { return m_scroll; }
    const PickupHud& pickups() const { return m_pickups; }
    const AmbientSounds& ambience() const { return m_ambience; }
    /** How far Sumner's beam of light has come up, 0 to 1. */
    f32 beamAlpha() const { return m_beamAlpha; }
    /** Whether the party is still materialising: held under the level's title until the start
     * camera has ridden in, or for the effect's life when there is no start camera. */
    bool spawning() const { return m_startCamera.active() || m_spawnTicks > 0; }
    usize spawnEffectCount() const { return m_spawns.size(); }
    /** The folder a character's figure came from, when it loaded. */
    std::optional<std::filesystem::path> figureDirectory(s32 player) const;
    /** Whether a character's figure carries its weapon. */
    bool weaponHeld(s32 player) const;
    /** The folder holding a character's figure at their level: the costume tier of ten
     * levels (`BLU00` for levels 1 to 9, `BLU10` for 10 to 19, and so on) when it is
     * unpacked, else the untiered costume. */
    static std::filesystem::path costumeDirectory(const std::filesystem::path& unpackedRoot,
                                                  const CharacterSave& save);
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
        std::filesystem::path directory;
        TreeInfo weaponTree; ///< one node: the weapon in the hand
        TreeModel weapon;
        s32 handNode = -1;   ///< the costume node the weapon hangs from
        std::vector<s32> classNodeOfNode; ///< per costume node: the class tree node it follows
        std::vector<Mat4> transforms;     ///< per costume node, from the current pose

        void animate(f32 stickMagnitude, s32 ticks, f32 seconds);
    };

    void spawnParty(std::span<const PartyMember> party, const TowerOptions& options);
    std::unique_ptr<Figure> loadFigure(RenderDevice& device, const CharacterSave& save);
    void loadActions(Figure& figure, const CharacterSave& save);
    static void loadWeapon(Figure& figure, const CharacterSave& save, RenderDevice& device);
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
    void updateAmbience();
    void updateBeam(s32 ticks);
    void beginSpawn(RenderDevice& device, bool ride);
    void updateSpawn(s32 ticks, f32 seconds);
    void drawSpawn(RenderDevice& device, const Mat4& clip) const;
    void drawLevelTitle(f32 width);
    bool anyButton(const Inputs& inputs) const;
    bool openMessage(std::string_view name, usize page);
    /** Plays a sound by name from whichever of the level's banks holds it; kNoSound when
     * none does. */
    SoundHandle playNamed(std::string_view name);
    void stopVoice();
    void stopOpeningSounds();
    void announceUnlock(s32 realm);
    void handleTriggerEvents();
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
    SoundSet m_levelBank;   ///< the realm's bank for the level
    SoundSet m_ambientBank; ///< the tower's ambience
    AmbientSounds m_ambience;
    std::array<std::optional<u32>, 2> m_stepSounds{};
    std::optional<u32> m_pickupSound; ///< one per foot
    SoundHandle m_music = kNoSound;
    SoundHandle m_voice = kNoSound; ///< Sumner's line over the scroll, cut when it is left
    /** A target opening before the party and the sound it makes meanwhile. */
    struct OpeningSound {
        s32 target = -1;
        SoundHandle handle = kNoSound;
    };
    std::vector<OpeningSound> m_openingSounds;
    SumnerFigure m_sumner;
    TextureSet m_staticTextures;
    BitmapFont m_font32;
    TextPainter m_text;
    MessageTable m_scrollText;
    ScrollBox m_scroll;
    Intro m_intro = Intro::None;
    WorldCamera m_cutCamera;
    s32 m_cutTicks = 0;
    s32 m_beam = -1; ///< the level object that is Sumner's beam of light
    f32 m_beamAlpha = 0.0f;

    /** A character materialising: the effect tree at their feet. */
    struct Spawn {
        Vec3 position{0.0f, 0.0f, 0.0f};
        const TreeInfo* tree = nullptr;
        TreeModel model;
        TreePose pose;
        AnimationPlayer player;
    };
    ItemArchive m_weapons; ///< holds the spawn effect
    std::vector<Spawn> m_spawns;
    TextureAnimator m_spawnTexmods;
    s32 m_spawnTicks = 0;
    f32 m_spawnFrames = 0.0f;
    StartCamera m_startCamera;
    f32 m_titleSlide = 0.0f; ///< how far the level's title has slid up the screen
    bool m_welcomePending = false;
};

} // namespace gdl::game
