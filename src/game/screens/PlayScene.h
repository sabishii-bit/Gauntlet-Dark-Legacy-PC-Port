#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/world/AmbientDimmer.h"
#include "engine/world/WorldCamera.h"

#include "game/players/CharacterSave.h"
#include "game/players/ClassData.h"
#include "game/players/LevelWatch.h"
#include "game/players/Party.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"
#include "game/players/TurboMeter.h"
#include "game/screens/BossVictoryPresentation.h"
#include "game/screens/GameContext.h"
#include "game/screens/LegendPresentation.h"
#include "game/screens/LevelArrivalPresentation.h"
#include "game/screens/LevelFixtures.h"
#include "game/screens/LevelMessages.h"
#include "game/screens/LevelOpponents.h"
#include "game/screens/PartyHud.h"
#include "game/screens/PartyMotion.h"
#include "game/screens/PlayerAttacks.h"
#include "game/screens/PlayerHealth.h"
#include "game/screens/SumnerVisit.h"
#include "game/screens/TransitionScreen.h"
#include "game/world/BossCamera.h"
#include "game/world/EffectTrees.h"
#include "game/world/ExitPortals.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"
#include "game/world/PlayerArsenal.h"
#include "game/world/PlayerFigure.h"
#include "game/world/StartCamera.h"
#include "game/world/SumnerFigure.h"
#include "game/world/TowerCamera.h"

namespace gdl::game {

/** An item dropped into the level by one of its item records' names. */
struct DroppedItem {
    std::string name;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/** How the tower may open other than as the game does, for tests and scenarios. */
struct PlayOptions {
    std::optional<Vec3> position;   ///< where the party stands instead of the entrance (the
                                    ///< start camera then does not ride in from its marker)
    std::optional<f32> yaw;         ///< the way it faces, instead of the entrance's
    std::optional<bool> welcome;    ///< whether Sumner welcomes it, else by its experience
    std::vector<DroppedItem> items; ///< dropped about the level once it opens
    u32 arrivalWorld = 0;           ///< the realm the party comes from, which picks the start point
                                    ///< it arrives at (none: the level's own entrance)
    bool arriving = false;          ///< the party comes through a portal: the transition picture is
                                    ///< up as the level opens, and clears
};

enum class PlayOutcome : u8 {
    Running,
    Leave,  ///< back to the title
    Travel, ///< through an exit portal to destination()
    Fallen  ///< everyone has died: back to the tower, where they stand again
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
    static constexpr s32 kSpawnTicks = LevelArrivalPresentation::kSpawnTicks;
    /** The voice that announces each realm's gate opening, by realm. */
    static constexpr std::array<std::string_view, 9> kUnlockVoices{
        "",           "S_CRYS4TWN", "S_CRYS4MNT", "S_CRYS4CST", "S_CRYS4SKY",
        "S_CRYS4FOR", "S_CRYS4DES", "S_CRYS4ICE", "S_CRYS4DRM"};
    static constexpr s32 kBeamFadeTicks = 180; ///< and how long it takes to come up or go
    static constexpr s32 kCrystalTicks = 300;  ///< fifty frames of six ticks
    static constexpr s32 kSumnerSpot = 240;    ///< the id of the trigger before him
    static constexpr f32 kGreetingSeconds = SumnerVisit::kGreetingSeconds;
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
    void render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth, f32 frameHeight);

    usize actorCount() const { return m_players.size(); }
    /** The character driven by `player`, or null when that player is not in the party. */
    const PlayerActor* actor(s32 player) const;
    /** The body animation of `player`'s character, or null without a figure for it. */
    const PlayerAnimator* animator(s32 player) const;
    const TowerCamera& camera() const { return m_camera; }
    /** The camera the scene is seen through: the start camera while it holds and rides in,
     * the crystals during the welcome's cut, else the follow camera. */
    const WorldCamera& viewCamera() const;
    /** The boss fight's camera, used while a boss level's boss stands. */
    const BossCamera& bossCamera() const { return m_bossCamera; }
    bool bossCameraOn() const;
    BossCameraSubject bossSubject() const;
    const StartCamera& startCamera() const { return m_arrival.camera(); }
    /** The music's voice, kNoSound while nothing plays. */
    SoundHandle music() const { return m_audio.music(); }
    /** Sumner's voice over the scroll he is reading, kNoSound while he is quiet. */
    SoundHandle voice() const { return m_audio.voice(); }
    /** The sound of the target that opened before the party most recently and is still
     * opening (a gate's force field humming as it thins, a lift, a gate), kNoSound otherwise. */
    SoundHandle fieldSound() const { return m_audio.fieldSound(); }
    Intro intro() const { return m_intro; }
    const ScrollBox& scroll() const { return m_messages.scroll(); }
    const HintMenu& hints() const { return m_sumnerVisit.menu(); }
    const PlayerMissiles& missiles() const { return m_arsenal.missiles(); }
    const ExitPortals& portals() const { return m_portals; }
    const Chests& chests() const { return m_fixtures.chests(); }
    const LockedGates& gates() const { return m_fixtures.gates(); }
    const Traps& traps() const { return m_fixtures.traps(); }
    const Breakables& barrels() const { return m_fixtures.barrels(); }
    const SafeRocks& safeRocks() const { return m_fixtures.safeRocks(); }
    const Enemies& enemies() const { return m_opponents.enemies(); }
    Enemies& enemies() { return m_opponents.enemies(); }
    const Generators& generators() const { return m_opponents.generators(); }
    Generators& generators() { return m_opponents.generators(); }
    const Critters& critters() const { return m_opponents.critters(); }
    Critters& critters() { return m_opponents.critters(); }
    const EnemyMissiles& enemyMissiles() const { return m_opponents.missiles(); }
    const Bosses& bosses() const { return m_opponents.bosses(); }
    Bosses& bosses() { return m_opponents.bosses(); }
    /** The boss's meter, when the level has one: its name and health for the HUD. */
    std::optional<BossView> bossView() const {
        return m_opponents.bosses().present() ? std::optional<BossView>(m_opponents.bosses().view())
                                              : std::nullopt;
    }
    const BossMeter& bossMeter() const { return m_opponents.meter(); }
    /** The wizard's visit once the boss has fallen. */
    const BossVictory& victory() const { return m_victory.state(); }
    /** The archive folder a character's figure was loaded from, for tests. */
    std::optional<std::filesystem::path> figureDirectory(usize index) const {
        return index < m_players.size() && m_players[index].figure != nullptr
                   ? std::optional<std::filesystem::path>(m_players[index].figure->directory())
                   : std::nullopt;
    }
    const HelpMessages& help() const { return m_hud.help(); }
    const MoveStrikes& strikes() const { return m_attacks.strikes(); }
    const AmbientDimmer& dimmer() const { return m_dimmer; }
    /** Gives `player`'s character experience won in play, which also feeds its turbo meter
     * (unless it is in the middle of a turbo move), as a kill does in the original. */
    void awardExperience(s32 player, s32 amount, bool kill = true);
    /** Harms `player`'s character as a blow, a burn, a piercing or gas would, for tests. */
    void harm(s32 player, f32 damage, HurtKind kind);
    /** What `player`'s status box shows. */
    StatusBoxView status(s32 player) const { return PartyHud::status(player, m_players); }
    /** The turbo meter of `player`'s character, or null when that player is not in. */
    const TurboMeter* turboMeter(s32 player) const;
    /** Whether `player`'s character has fallen (dying or gone to the tower). */
    bool fallen(s32 player) const;
    /** Hurts `player`'s character, as anything in the level does. */
    void hurtPlayer(s32 player, f32 damage, HurtKind kind, bool directed = false);
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
    const PowerupSelector& selector(s32 player) const { return m_hud.selector(player); }
    /** How large a character is drawn: an ogre, one grown by a powerup, one of level 99. */
    static f32 bodyScale(const CharacterSave& save, const PowerupEffects& effects);
    const SumnerHints& hintTexts() const { return m_sumnerVisit.texts(); }
    const PickupHud& pickups() const { return m_hud.pickups(); }
    const AmbientSounds& ambience() const { return m_audio.ambience(); }
    /** How far Sumner's beam of light has come up, 0 to 1. */
    f32 beamAlpha() const { return m_beamAlpha; }
    /** Whether the party is still materialising: held under the level's title until the start
     * camera has ridden in, or for the effect's life when there is no start camera. */
    bool spawning() const { return m_arrival.active(); }
    usize spawnEffectCount() const { return m_arrival.effectCount(); }
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
    void spawnParty(std::span<const PartyMember> party, const PlayOptions& options);
    void throwWeapon(const PlayerActor& actor);
    static bool freshParty(std::span<const PartyMember> party);
    void beginIntro(RenderDevice& device);
    void startCrystalCut();
    u32 acceptedPlayers(const Inputs& inputs) const;
    std::vector<TriggerVisitor> visitors() const;
    void collectItems();
    bool leaveBy(usize portal);
    void updateFixtures(s32 ticks, f32 seconds);
    LevelFixtures::Events fixtureEvents();
    PlayerAttacks::Targets attackTargets();
    void hurtOpponentsByBlast(const Vec3& position, f32 radius, f32 damage);
    void hurt(usize index, f32 damage, HurtKind kind, bool directed = false);
    void updateEnemies(s32 ticks, f32 seconds);
    void strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer);
    void strikeGenerator(s32 id, f32 power, s32 byPlayer);
    void strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                       std::optional<Vec3> where = std::nullopt, bool close = false);
    /** The blast a boss's death lets off, which nothing of the swarm survives. */
    static constexpr f32 kBossDeathBlast = 1000.0f;
    static constexpr f32 kBossDeathBlastRadius = 1000.0f;
    void spewBossCoins(const CritterSpew& spew);
    void showLegendEvent(const LegendEvent& event);
    void updateLegend(f32 seconds);
    std::optional<LegendPresentation::Bearer> legendBearer(s32 player, s32 kind) const;
    void bossFallen(const Vec3& where);
    void updateVictory(s32 ticks, f32 seconds);
    void settleBlasts();
    bool postHelp(s32 id, usize index, s32 number = -1);
    /** Answers the party's levels gained since last looked: the fanfare, a hundred health,
     * the message, the costume of a new tier, and the class's word at a milestone. */
    void updateLevels();
    static constexpr f32 kLevelUpHealth = 100.0f;
    void sayWithName(usize index, std::string_view line);
    void launchWeapon(usize index, const Vec3& direction, f32 scale, bool spreads);
    bool isDown(usize index) const {
        return index < m_players.size() && m_players[index].life != PlayerLife::Standing;
    }
    /** Where the level finds a character: nowhere once it has fallen. */
    Vec3 presenceOf(usize index) const;
    std::optional<s32> takePickup(const Pickup& pickup);
    void shareRune(s32 rune);
    void updateAmbience();
    void updateBeam(s32 ticks);
    void beginSpawn(RenderDevice& device, bool ride);
    bool anyButton(const Inputs& inputs) const;
    bool openMessage(std::string_view name, usize page);
    void announceUnlock(s32 realm);
    void handleTriggerEvents();
    const PlayerActor* visitorOfSumner() const;
    void updateSumnerVisit(f32 seconds);
    void updateHints(const Inputs& inputs, s32 ticks);
    CameraView cameraView() const;

    bool m_open = false;
    RenderDevice* m_device = nullptr;
    GameContext m_context;
    LevelWorld* m_world = nullptr;
    ClassDataSet m_classes;
    PartyHud m_hud;
    Canvas m_canvas;
    TowerCamera m_camera;
    BossCamera m_bossCamera;
    std::vector<PlayerRuntime> m_players;
    LevelSoundscape m_audio;
    SumnerFigure m_sumner;
    TextureSet m_staticTextures;
    LevelMessages m_messages;
    PlayerArsenal m_arsenal;
    ExitPortals m_portals;
    PlayerHealth m_health;
    std::mt19937 m_coinRandom{0xC01Eu}; ///< how fast each coin a boss spews flies
    AmbientDimmer m_dimmer;
    PlayerAttacks m_attacks;
    LevelFixtures m_fixtures;
    LevelOpponents m_opponents;
    BossVictoryPresentation m_victory;
    LevelWatch m_levels;
    TransitionScreen m_transition;
    LevelRef m_destination;
    s32 m_refusedPortal = -1; ///< the portal last found to lead nowhere, not to say so twice
    EffectTrees m_effects;
    std::unique_ptr<LegendPresentation> m_legend; ///< destroyed before its borrowed effect store
    f32 m_playSeconds = 0.0f;
    f32 m_fallenSeconds = 0.0f; ///< since the last of the party fell
    SumnerVisit m_sumnerVisit;
    WorldCamera m_cutCamera;
    s32 m_cutTicks = 0;
    s32 m_beam = -1; ///< the level object that is Sumner's beam of light
    f32 m_beamAlpha = 0.0f;

    ItemArchive m_weapons;              ///< shared weapon and effect assets
    LevelArrivalPresentation m_arrival; ///< borrows the weapons archive
    bool m_welcomePending = false;
    bool m_leaving = false;
    Intro m_intro = Intro::None;
};

} // namespace gdl::game
