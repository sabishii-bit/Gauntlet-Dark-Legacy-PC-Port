#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
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
#include "engine/ui/ModelSprite.h"
#include "engine/ui/TextPainter.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"

#include "game/menu/HintMenu.h"
#include "game/menu/MenuInput.h"
#include "game/menu/ScrollBox.h"
#include "game/players/CharacterSave.h"
#include "game/players/Party.h"
#include "game/players/ClassData.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PlayerControls.h"
#include "game/screens/GameContext.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/PickupHud.h"
#include "game/screens/PowerupSelector.h"
#include "game/screens/StatusBox.h"
#include "game/screens/TransitionScreen.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreePose.h"

#include "game/players/PowerupEffects.h"
#include "game/world/AmbientSounds.h"
#include "game/world/Breakables.h"
#include "game/world/Chests.h"
#include "game/world/LockedGates.h"
#include "game/world/Traps.h"
#include "game/world/EffectTrees.h"
#include "game/world/ExitPortals.h"
#include "game/world/PlayerMissiles.h"
#include "game/world/StartCamera.h"
#include "game/world/SumnerFigure.h"
#include "game/world/SumnerHints.h"
#include "game/world/TowerCamera.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {

/** An item dropped into the level by one of its item records' names. */
struct DroppedItem {
    std::string name;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/** How the tower may open other than as the game does, for tests and scenarios. */
struct PlayOptions {
    std::optional<Vec3> position; ///< where the party stands instead of the entrance (the
                                  ///< start camera then does not ride in from its marker)
    std::optional<f32> yaw;       ///< the way it faces, instead of the entrance's
    std::optional<bool> welcome;  ///< whether Sumner welcomes it, else by its experience
    std::vector<DroppedItem> items; ///< dropped about the level once it opens
    u32 arrivalWorld = 0; ///< the realm the party comes from, which picks the start point
                          ///< it arrives at (none: the level's own entrance)
    bool arriving = false; ///< the party comes through a portal: the transition picture is
                           ///< up as the level opens, and clears
};

/** One player's input for a frame of play. */
struct PlayInput {
    MoveInput move;
    MenuInput menu;
    bool attack = false; ///< the attack button is held
    bool usePotion = false;
    bool throwPotion = false;
    SelectorInput selector; ///< this frame's presses for the powerup selector
};

enum class PlayOutcome : u8 {
    Running,
    Leave,  ///< back to the title
    Travel, ///< through an exit portal to destination()
    Fallen  ///< everyone has died: back to the tower, where they stand again
};

/** What hurt a character, which picks how it cries out. */
enum class HurtKind : u8 {
    Blow,   ///< cries out once enough has been taken
    Burn,   ///< always cries out
    Pierce, ///< groans
    Gas     ///< coughs
};

/**
 * The tower with the party in it: each character stands at the entrance and walks under its
 * player's control, its body playing the class's sequences and its feet sounding on the stone,
 * the camera follows from the level's marker angles, the level's music loops from its stream,
 * Sumner idles at his lookout, and the status boxes line the bottom of the screen. The party
 * materialises held still, playing its entrance, under the level's title while the start
 * camera holds at the entrance marker and rides in to the follow camera. A party new to the
 * tower is then welcomed the way the original does it: his scroll of pages first, then his
 * gesture as the camera cuts to the crystals for five seconds, between black bars, with the
 * party held still. A player who steps up to Sumner is greeted and, two seconds on, handed
 * his scroll of hints, which holds play until it has burnt away; he waves them off after.
 * Holding the attack throws the character's weapon, again and again: the body winds up and
 * lets go where it stands, the weapon flies from the hand's side along its facing, and the
 * hand is empty until the throw is recovered from. A potion carried is used where the
 * character stands, its magic bursting about them, or thrown a little way to burst where it
 * lands. The powerups carried are named by the selector over the status box and, while worn,
 * quicken the character, spread their throws three or five ways, leave them half unseen or
 * a third larger. A level's chests and locked gates stand solid in the way until someone
 * against them spends a key, a chest then giving up what it held; its traps hurt whoever is
 * in them while they are out, by the level's own scales and the difficulty's; its barrels
 * break under thrown weapons and blasts, giving up what they held, blowing up or leaving a
 * cloud of gas. The original's help messages go up over a character the first time each
 * thing happens. A character whose health runs out falls and waits in the tower, its box
 * saying so; it comes back, as it entered the level, when the party returns there, and when
 * everyone has fallen the party is taken back.
 */
class PlayScene {
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
    static constexpr s32 kSumnerSpot = 240;    ///< the id of the trigger before him
    static constexpr f32 kGreetingSeconds = 2.0f; ///< from his greeting to his scroll
    using Inputs = std::array<PlayInput, kPlayerCount>;

    /** Where a new party's welcome has got to. */
    enum class Intro : u8 { None, Scroll, Crystal, Done };

    /** Brings the party into `world` (loading it when needed); false when the level or the
     * status boxes are not unpacked. */
    bool open(RenderDevice& device, const GameContext& context, LevelWorld& world,
              std::span<const PartyMember> party, const PlayOptions& options = {});
    void close();
    bool isOpen() const { return m_open; }

    PlayOutcome update(f64 deltaSeconds, const Inputs& inputs);
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
    const HintMenu& hints() const { return m_hintMenu; }
    const PlayerMissiles& missiles() const { return m_missiles; }
    const ExitPortals& portals() const { return m_portals; }
    const Chests& chests() const { return m_chests; }
    const LockedGates& gates() const { return m_gates; }
    const Traps& traps() const { return m_traps; }
    const Breakables& barrels() const { return m_barrels; }
    const HelpMessages& help() const { return m_help; }
    /** Whether `player`'s character has fallen (dying or gone to the tower). */
    bool fallen(s32 player) const;
    /** Hurts `player`'s character, as anything in the level does. */
    void hurtPlayer(s32 player, f32 damage, HurtKind kind);
    /** A blast at `position`: hurts and breaks what is within `radius`. */
    void blast(const Vec3& position, f32 radius, f32 damage);
    const TransitionScreen& transition() const { return m_transition; }
    /** Whether the party has gone through a portal and the picture is coming up over it. */
    bool leaving() const { return m_leaving; }
    /** Where the party is bound once update() has said Travel, and the realm it leaves. */
    const LevelRef& destination() const { return m_destination; }
    /** The party as it stands, with all it has gathered, for the next level. */
    std::vector<PartyMember> party() const;
    const EffectTrees& effects() const { return m_effects; }
    /** The powerup selector over `player`'s box. */
    const PowerupSelector& selector(s32 player) const {
        return m_selectors[static_cast<usize>(std::clamp(player, 0, kPlayerCount - 1))];
    }
    /** How large a character is drawn: an ogre, one grown by a powerup, one of level 99. */
    static f32 bodyScale(const CharacterSave& save, const PowerupEffects& effects);
    const SumnerHints& hintTexts() const { return m_hints; }
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
        ItemArchive effects; ///< the costume colour's effects, when the thrown weapon is there
        TreeModel missile;   ///< the weapon as it flies
        SoundSet voice;      ///< the class's own sounds
        std::optional<u32> throwSound;

        void animate(f32 stickMagnitude, s32 ticks, f32 seconds,
                     PlayerDeed deed = PlayerDeed::None);
    };

    void spawnParty(std::span<const PartyMember> party, const PlayOptions& options);
    std::unique_ptr<Figure> loadFigure(RenderDevice& device, const CharacterSave& save);
    void loadActions(Figure& figure, const CharacterSave& save);
    static void loadWeapon(Figure& figure, const CharacterSave& save, RenderDevice& device);
    void loadMissile(Figure& figure, const CharacterSave& save, RenderDevice& device);
    void throwWeapon(const PlayerActor& actor, Figure& figure);
    void loadPotionModels(RenderDevice& device);
    void usePotion(PlayerActor& actor);
    void throwPotion(PlayerActor& actor);
    void burstPotion(s32 kind, const Vec3& position, f32 power);
    f32 magicPowerOf(const PlayerActor& actor) const;
    void stepSelector(PlayerActor& actor, const SelectorInput& input, s32 ticks);
    void drawSelectors();
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
    bool leaveBy(usize portal);
    void updateFixtures(s32 ticks, f32 seconds);
    void playGateSound(s32 subtype);
    void hurt(usize index, f32 damage, HurtKind kind);
    void strikeBarrel(usize barrel, f32 power, s32 byPlayer);
    void settleBlasts();
    void updateClouds(f32 seconds);
    bool postHelp(s32 id, usize index);
    SoundHandle playRealmSound(std::string_view stem);
    void cry(usize index, std::string_view which);
    bool isDown(usize index) const { return index < m_down.size() && m_down[index] != kUp; }
    /** Where the level finds a character: nowhere once it has fallen. */
    Vec3 presenceOf(usize index) const;
    f32 trapDamageScale() const;
    void drawHelp(const Mat4& clip, f32 width, f32 height);
    std::optional<s32> takePickup(const Pickup& pickup);
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
    void loadHintArt(RenderDevice& device);
    const PlayerActor* visitorOfSumner() const;
    void updateSumnerVisit(f32 seconds);
    void openHints(s32 player);
    void updateHints(const Inputs& inputs, s32 ticks);
    void answerHint(s32 topic);
    StatusBoxView statusOf(s32 player) const;
    CameraView cameraView() const;

    bool m_open = false;
    RenderDevice* m_device = nullptr;
    GameContext m_context;
    LevelWorld* m_world = nullptr;
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
    PlayerMissiles m_missiles;
    ExitPortals m_portals;
    std::vector<std::optional<usize>> m_slots; ///< each actor's save slot, when it has one
    static constexpr u8 kUp = 0;
    static constexpr u8 kDying = 1;
    static constexpr u8 kInTower = 2;
    std::vector<u8> m_down;                  ///< per actor
    std::vector<CharacterSave> m_entrySaves; ///< per actor, as it came into the level
    std::vector<f32> m_painOwed;             ///< per actor, harm not yet cried out over
    std::vector<PlayerDeed> m_struck;        ///< per actor, the reaction a hit this tick asks
    /** Gas a poison barrel left hanging. */
    struct GasCloud {
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 damage = 0.0f;
        f32 secondsLeft = 0.0f;
    };
    std::vector<GasCloud> m_clouds;
    /** A blast yet to be felt: one barrel's sets off the next, in turn. */
    struct Blast {
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 radius = 0.0f;
        f32 damage = 0.0f;
    };
    std::vector<Blast> m_blasts;
    std::vector<f32> m_cloudGaps; ///< per actor, seconds before gas hurts them again
    Breakables m_barrels;
    HelpMessages m_help;
    MessageTable m_strings;  ///< the game's own strings, which hold the help messages
    SoundSet m_narrator;     ///< who says them
    Chests m_chests;
    LockedGates m_gates;
    Traps m_traps;
    TransitionScreen m_transition;
    bool m_leaving = false;
    LevelRef m_destination;
    s32 m_refusedPortal = -1; ///< the portal last found to lead nowhere, not to say so twice
    EffectTrees m_effects;
    std::array<TreeModel, 5> m_potionModels; ///< a thrown potion, by its kind
    std::array<PowerupSelector, kPlayerCount> m_selectors;
    const Texture* m_glowSheet = nullptr; ///< the glow a worn powerup's name is written in
    f32 m_playSeconds = 0.0f;
    f32 m_fallenSeconds = 0.0f; ///< since the last of the party fell
    SumnerHints m_hints;
    HintMenu m_hintMenu;
    ModelSprite m_hintArrow;
    s32 m_hintPlayer = -1;       ///< whose scroll of hints is out
    f32 m_greetingLeft = -1.0f;  ///< seconds from his greeting to his scroll; negative: none
    bool m_hintsGiven = false;   ///< this visit has had its scroll; leaving him clears it
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
