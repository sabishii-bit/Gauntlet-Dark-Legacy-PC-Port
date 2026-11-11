#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/world/AmbientDimmer.h"
#include "engine/world/WorldCamera.h"

#include "game/audio/LevelSoundscape.h"
#include "game/bosses/BossSequence.h"
#include "game/camera/BossCamera.h"
#include "game/camera/CameraShake.h"
#include "game/camera/StartCamera.h"
#include "game/camera/TowerCamera.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/LevelResults.h"
#include "game/characters/LevelWatch.h"
#include "game/characters/Party.h"
#include "game/config/ControlProfiles.h"
#include "game/context/GameServices.h"
#include "game/effects/EffectTrees.h"
#include "game/enemies/LevelOpponents.h"
#include "game/hud/ChallengeHud.h"
#include "game/hud/CompanionArrows.h"
#include "game/hud/CompassHud.h"
#include "game/hud/LevelMessages.h"
#include "game/hud/PartyHud.h"
#include "game/hud/RuneMeter.h"
#include "game/level/ExitPortals.h"
#include "game/level/FixtureHost.h"
#include "game/level/LevelFixtures.h"
#include "game/level/LevelTransporters.h"
#include "game/level/LevelWorld.h"
#include "game/level/NativeStateResources.h"
#include "game/level/SecretChallenge.h"
#include "game/level/SpawnedObjects.h"
#include "game/level/SwitchCutscene.h"
#include "game/level/TowerAccess.h"
#include "game/level/TriggerCues.h"
#include "game/mechanics/MechanicsFacade.h"
#include "game/party/AttackHost.h"
#include "game/party/PartyFigures.h"
#include "game/party/PartyMotion.h"
#include "game/party/PartyNames.h"
#include "game/party/PartyPickups.h"
#include "game/party/PickupPresentation.h"
#include "game/party/PlayerArsenal.h"
#include "game/party/PlayerAttacks.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PlayerHealth.h"
#include "game/party/PortalDeparture.h"
#include "game/players/PlayerActor.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/TurboMeter.h"
#include "game/rollback/ConsoleAction.h"
#include "game/rollback/FrameEventLedger.h"
#include "game/rollback/InputCommand.h"
#include "game/rollback/SessionInputs.h"
#include "game/scenes/ClassUnlocks.h"
#include "game/scenes/Haptics.h"
#include "game/scenes/ScenePicking.h"
#include "game/scenes/SceneSelection.h"
#include "game/scenes/SeatCameras.h"
#include "game/scenes/TowerCeremonies.h"
#include "game/scenes/TransitionScreen.h"
#include "game/tower/GameOver.h"
#include "game/tower/LevelArrivalPresentation.h"
#include "game/tower/SumnerFigure.h"
#include "game/tower/SumnerVisit.h"
#include "game/tower/TowerWelcome.h"

namespace gdl::game {

/** An item dropped into the level by one of its item records' names. */
struct DroppedItem {
    std::string name;
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/** What the developer console grants a seat. Native values, not local console-window
 * preferences: rewound with the scene. */
struct ConsoleCheats {
    bool god = false;
    bool noTarget = false;
    bool noclip = false;
    template <class Visitor>
    friend void visitNativeState(Visitor& visitor, const ConsoleCheats& value) {
        visitor.template fields<3>(value);
    }
    bool operator==(const ConsoleCheats&) const = default;
};

/** How the tower may open other than as the game does, for tests and scenarios. */
struct PlayOptions {
    static constexpr usize kSeats = 4; ///< the seats a scene plays (PlayScene::kPlayerCount)

    std::optional<Vec3> position;   ///< where the party stands instead of the entrance (the
                                    ///< start camera then does not ride in from its marker)
    std::optional<f32> yaw;         ///< the way it faces, instead of the entrance's
    std::optional<bool> welcome;    ///< whether Sumner welcomes it, else by its experience
    std::vector<DroppedItem> items; ///< dropped about the level once it opens
    u32 arrivalWorld = 0;           ///< the realm the party comes from, which picks the start point
                                    ///< it arrives at (none: the level's own entrance)
    bool arriving = false;          ///< the party comes through a portal (the level still cuts
                                    ///< in, as the original's does; kept for the online setup)
    /** The party size the level sets itself up for (its items, gates and generators) instead
     * of the players present: the console's party count; none follows the players. */
    std::optional<s32> partyCount;
    /** Each seat's console cheats, which the scene holds from its first tick. */
    std::array<ConsoleCheats, kSeats> consoleCheats{};
};

/** The application's owners an open scene answers through: the controllers' rumble, the
 * classes a won challenge grants and the Stop Time total. Any may be missing; closing the scene
 * lets them all go. */
struct PlaySceneLinks {
    Haptics* haptics = nullptr;
    ClassUnlocks* unlocks = nullptr;
    /** The most recent Stop Time pickup's total, shared across the levels a run visits; without
     * it the scene keeps its own. */
    f32* stopTimeTotal = nullptr;
};

enum class PlayOutcome : u8 {
    Running,
    Leave,   ///< back to the title
    Travel,  ///< through an exit portal to destination()
    GameOver ///< the last death and defeat caption are complete: leave gameplay
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
 * everyone has fallen the game-over sequence ends the session.
 */
class PlayScene {
public:
    static constexpr s32 kPlayerCount = 4;
    static_assert(static_cast<usize>(kPlayerCount) == PlayOptions::kSeats);
    using ConsoleCheats = game::ConsoleCheats;
    struct ConsoleTravelRequest {
        LevelRef destination;
        std::optional<Vec3> position;
        std::optional<f32> yaw;
        template <class Visitor>
        friend void visitNativeState(Visitor& visitor, const ConsoleTravelRequest& value) {
            visitor.template fields<3>(value);
        }
        bool operator==(const ConsoleTravelRequest&) const = default;
    };

private:
    /** Live, value-owned control state. Pure gameplay helpers are copied directly,
     * not translated into a second checkpoint schema. Assets, devices, callbacks
     * and persistent save-slot identities stay in their existing owners. */
    struct Simulation {
        friend void visitNativeState(NativeStateDigest& digest, const Simulation& value) {
            digest.fields<29>(value);
        }
        BossCamera bossCamera;
        GameOver gameOver;
        SecretChallenge challenge;
        RuneMeter runeMeter;
        std::optional<usize> runeItem;
        bool secretTravel = false;
        Vec3 secretReturnPosition{0.0f};
        SwitchCutscene switchCutscene;
        PlayerHealth health;
        PartyPickups pickups;
        AmbientDimmer dimmer;
        LevelWatch levels;
        LevelRef destination;
        s32 refusedPortal = -1;  ///< the portal last found to lead nowhere, not to say so twice
        TowerAccess towerAccess; ///< what the tower opens to this party; empty elsewhere
        f32 playSeconds = 0.0f;
        f32 towerNoticeSeconds = 0.0f;
        CameraShake shake;
        TowerWelcome welcome;
        bool leaving = false;
        bool opponentsAdvanced = false;
        bool projectilesAdvanced = false;
        bool movementUpdated = false;
        std::array<ConsoleCheats, static_cast<usize>(kPlayerCount)> cheats{};
        std::optional<ConsoleTravelRequest> consoleTravel;
        u32 consoleSpawns = 0;
        std::optional<s32> partyCountOverride; ///< the console's party count, when set
        bool soloCameras = false; ///< each seat looks through its own camera (a netplay rule)
        bool fixedRoster = false; ///< nobody leaves the party before it ends (a netplay rule)

        bool operator==(const Simulation&) const = default;
    };

public:
    static constexpr f32 kSpawnSpacing = 2.0f; ///< between party members at the entrance
    static constexpr s32 kSpawnTicks = LevelArrivalPresentation::kSpawnTicks;
    static constexpr s32 kCrystalTicks = TowerWelcome::kCrystalTicks;
    static constexpr f32 kGreetingSeconds = SumnerVisit::kGreetingSeconds;
    using Inputs = std::array<PlayInput, kPlayerCount>;
    static_assert(static_cast<usize>(kPlayerCount) == kInputSlots &&
                      static_cast<usize>(kPlayerCount) == InputCommand::kSeats,
                  "offline, seat n reads input slot n");
    static_assert(std::is_same_v<Inputs, SessionInputs::Frame>);

    using Intro = TowerWelcome::Intro;

    using Links = PlaySceneLinks;

    /** Brings the party into `world` (loading it when needed); false when the level or the
     * status boxes are not unpacked. The scene keeps reading `services`, which must outlive
     * it. */
    bool open(RenderDevice& device, const GameServices& services, LevelWorld& world,
              std::span<const PartyMember> party, const PlayOptions& options = {},
              const Links& links = {});
    /** The scene keeps the services it is opened with, so they may not be a temporary. */
    bool open(RenderDevice&, const GameServices&&, LevelWorld&, std::span<const PartyMember>,
              const PlayOptions& = {}, const Links& = {}) = delete;
    void close();
    bool isOpen() const { return m_open; }
    u64 loadGeneration() const { return m_openGeneration; }

    /** Offline play's tick with no console actions: step()'s outcome. */
    PlayOutcome update(f64 deltaSeconds, const Inputs& inputs);
    struct RumbleOutput {
        s32 player = 0;
        s32 frames = 0;
        ControlFeedback feedback = ControlFeedback::Damage;
        bool operator==(const RumbleOutput&) const = default;
    };
    struct UnlockClassesOutput {
        u16 mask = 0;
        bool operator==(const UnlockClassesOutput&) const = default;
    };
    using OutputPayload = std::variant<RumbleOutput, UnlockClassesOutput>;
    using OutputLedger = FrameEventLedger<OutputPayload>;
    struct ConsoleReply {
        u8 seat = 0;
        std::string text;
    };
    /** Which game a console action runs in: offline play on this machine alone, or the game
     * every peer steps alike. It picks the reply's wording and the shared game's own limits. */
    enum class ConsolePlay : u8 { Offline, Shared };
    struct RecordedStep {
        PlayOutcome outcome = PlayOutcome::Running;
        OutputLedger::Journal journal;
        std::optional<SceneSoundEvents::Frame> audio;
        std::vector<ConsoleReply> consoleReplies;
    };
    /** Captures rumble and persistent class-unlock intents without invoking those
     * application callbacks. Native character rewards still change immediately.
     * Epoch/tick must be nonzero; invalid or nested calls throw before advancing.
     * The caller owns the fixed-roster epoch and supplies the completed tick.
     * Audio is journaled only after beginRecordedAudio; otherwise the offline
     * sound path remains immediate. Callers must explicitly select that boundary.
     * The console actions run in the shared game. */
    RecordedStep recordStep(u64 epoch, u64 tick, f64 deltaSeconds, const Inputs& inputs,
                            std::span<const ConsoleAction> actions = {});
    /** What an offline tick came to, with the replies to its console actions in their order. */
    struct LocalStep {
        PlayOutcome outcome = PlayOutcome::Running;
        std::vector<ConsoleReply> replies;
    };
    /** Offline play's tick: the console actions run first, in offline play's game, and the
     * tick's outputs are dispatched here. Throws while the scene records its audio. */
    LocalStep step(f64 deltaSeconds, const Inputs& inputs, std::span<const ConsoleAction> actions);
    /** Native audio gate, before the first snapshot of a loaded online epoch.
     * Rollback requires this dispatcher; offline play keeps immediate audio. */
    bool beginRecordedAudio(u64 epoch);
    /** Presentation boundary only, after abandoning history or confirming travel.
     * Ending recording invalidates snapshots from its audio epoch. */
    void endRecordedAudio();
    /** For a scene returned to the state it began `epoch` in: abandons that epoch's recorded
     * history and records the same epoch again from its first tick. */
    bool restartRecordedAudio(u64 epoch);
    u64 recordedAudioEpoch() const { return m_recordedAudio.epoch(); }
    const LevelSoundscape& audio() const { return m_audio; }
    SceneSoundEvents::Result reconcileRecordedAudio(u64 firstTick,
                                                    std::span<const SceneSoundEvents::Frame> frames,
                                                    std::optional<u64> newHead = std::nullopt);
    SceneSoundEvents::Result confirmRecordedAudio(u64 allSeatsThrough);
    void dispatchRecordedAudio(const SceneSoundEvents::Result& result);
    void serviceRecordedAudio(f32 seconds);
    /** Dispatch a ledger-approved rumble Present or unlock Commit once. Online
     * callers must obtain Commit only from the all-seat-confirmed frontier.
     * Offline update dispatches its fresh intents immediately after the step. */
    bool dispatchOutput(const OutputLedger::Action& action) const;
    /** In-process history for this loaded scene. Copies the live value owners,
     * never serializes resources or restores across a load/party change. Output
     * and persistent writes must be gated by the caller before speculative use. */
    class LocalSnapshot {
    public:
        LocalSnapshot();
        ~LocalSnapshot();
        LocalSnapshot(LocalSnapshot&&) noexcept;
        LocalSnapshot& operator=(LocalSnapshot&&) noexcept;
        LocalSnapshot(const LocalSnapshot&) = delete;
        LocalSnapshot& operator=(const LocalSnapshot&) = delete;

    private:
        friend class PlayScene;
        struct Data;
        std::unique_ptr<Data> m_data;
    };
    bool saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    /** Read-only exact value equality of root control state, including hidden
     * helper clocks, coin identities and RNG. Not whole-scene equality: players,
     * world, other owners and outputs are outside this comparison. Uses normal
     * typed equality (including float ==), not byte/signed-zero comparison. */
    bool matchesLocalControl(const LocalSnapshot& saved) const;
    /** Resolve a local pointer against the currently presented Sumner menu.
     * Read-only: only the bounded semantic choice enters shared native input. */
    PlayInput captureNativeInput(s32 player, PlayInput input) const;
    /** Silence ambience and drain loops before the options menu stops gameplay updates. */
    void pauseGameplaySounds();
    /** The pause menu holds the level's sounds and speech where they are, and lets them go. */
    void holdSounds() { m_audio.holdSounds(); }
    void releaseSounds() { m_audio.releaseSounds(); }
    /** Application boundary for parking a live scene behind an imported level.
     * Stops only this scene's voices; never holds the shared effects category. */
    void suspendForImportedLevel(bool preserveImportedSpeech = false);
    void resumeAfterImportedLevel();
    /** Recover an offline imported exit whose destination could not be loaded. */
    bool recoverImportedDeparture();
    void render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth, f32 frameHeight,
                bool optionsOpen = false, f32 frameBlend = -1.0f);

    usize actorCount() const { return m_players.size(); }
    /** Read-only end-of-tick observation; callers must not retain the span across open/close. */
    std::span<const PlayerRuntime> participants() const { return m_players; }
    /** Periodic native agreement watchdog, without replica bindings or state
     * transfer. Observes party outcomes/motion, opponent pools and scene control;
     * it is not a complete serialization of every hidden simulation owner. */
    struct NativeDigestProfile {
        struct Sample {
            std::string_view owner;
            f64 microseconds = 0;
            /** The digest of the stream up to and including this owner; empty once the
             * stream or this owner fails. */
            std::optional<NativeStateDigest::Digest> runningDigest;
        };
        std::vector<Sample> samples;
    };
    /** Optional diagnostics only; the ordinary path does not read a clock. */
    std::optional<NativeStateDigest::Digest>
    nativeAgreementDigest(NativeDigestProfile* profile = nullptr);
    /** Same schema over retained native values, without restoring or drawing. */
    std::optional<NativeStateDigest::Digest> nativeLocalDigest(const LocalSnapshot& saved);
    /** Read-only native load-barrier preflight for message/hint resources whose
     * absence changes simulation holds. Does not change offline optional art. */
    bool nativePresentationReady() const;
    const LevelWorld* world() const { return m_world; }
    static CameraView cameraView();
    /** Hard-cut rule used by local render interpolation. */
    bool cameraContinuous() const;
    u32 simulationTickRate() const {
        return m_services->config != nullptr ? m_services->config->timing.tickRate : 60;
    }
    /** The character driven by `player`, or null when that player is not in the party. */
    const PlayerActor* actor(s32 player) const;
    /** The body animation of `player`'s character, or null without a figure for it. */
    const PlayerAnimator* animator(s32 player) const;
    const TowerCamera& camera() const { return m_camera; }
    /** The camera the scene is seen through: the start camera while it holds and rides in,
     * the crystals during the welcome's cut, else the follow camera. */
    WorldCamera viewCamera() const;
    /** Shared gameplay camera, never the local developer free camera. */
    WorldCamera simulationCamera() const;
    /** Horizontal world direction, zero in the cursor dead zone; never a world target. */
    std::optional<Vec3> cursorAim(Vec2 cursor, const Vec3& position) const;
    /** The boss fight's camera, used while a boss level's boss stands. */
    const BossCamera& bossCamera() const { return m_sim.bossCamera; }
    bool bossCameraOn() const;
    BossCameraSubject bossSubject() const;
    const StartCamera& startCamera() const { return m_arrival.camera(); }
    /** The music's voice, kNoSound while nothing plays. */
    SoundHandle music() const { return m_audio.music(); }
    const LevelSoundscape& soundscape() const { return m_audio; }
    /** Sumner's voice over the scroll he is reading, kNoSound while he is quiet. */
    SoundHandle voice() const { return m_audio.voice(); }
    /** The sound of the target that opened before the party most recently and is still
     * opening (a gate's force field humming as it thins, a lift, a gate), kNoSound otherwise. */
    SoundHandle fieldSound() const { return m_audio.fieldSound(); }
    bool exitFlameOn() const { return m_audio.exitFlameOn(); }
    Intro intro() const { return m_sim.welcome.intro(); }
    const ScrollBox& scroll() const { return m_messages.scroll(); }
    std::optional<LevelMessages::Look> scrollLook() const { return m_messages.look(); }
    const HintMenu& hints() const { return m_sumnerVisit.menu(); }
    const PlayerMissiles& missiles() const { return m_arsenal.missiles(); }
    PlayerArsenal::SoundCapture captureWeaponSounds(PlayerArsenal::SoundObserver observer) {
        return {&m_arsenal, std::move(observer)};
    }
    bool projectilesAdvanced() const { return m_sim.projectilesAdvanced; }
    const ExitPortals& portals() const { return m_portals; }
    const LevelTransporters& transporters() const { return m_transporters; }
    const PortalDeparture& departure() const { return m_departure; }
    const SwitchCutscene& switchCutscene() const { return m_sim.switchCutscene; }
    const LevelFixtures& fixtures() const { return m_fixtures; }
    const Chests& chests() const { return m_fixtures.chests(); }
    const LockedGates& gates() const { return m_fixtures.gates(); }
    const Traps& traps() const { return m_fixtures.traps(); }
    const Breakables& barrels() const { return m_fixtures.barrels(); }
    const SafeRocks& safeRocks() const { return m_fixtures.safeRocks(); }
    const Rubble& rubble() const { return m_fixtures.rubble(); }
    const LevelOpponents& opponents() const { return m_opponents; }
    const Enemies& enemies() const { return m_opponents.enemies(); }
    Enemies& enemies() { return m_opponents.enemies(); }
    const Generators& generators() const { return m_opponents.generators(); }
    Generators& generators() { return m_opponents.generators(); }
    const PlacedEnemies& placedEnemies() const { return m_opponents.placements(); }
    const Critters& critters() const { return m_opponents.critters(); }
    const CritterStatues& statues() const { return m_opponents.statues(); }
    Critters& critters() { return m_opponents.critters(); }
    const EnemyMissiles& enemyMissiles() const { return m_opponents.missiles(); }
    const Bosses& bosses() const { return m_opponents.bosses(); }
    Bosses& bosses() { return m_opponents.bosses(); }
    /** The boss's meter, when the level has one: its name and health for the HUD. */
    std::optional<BossView> bossView() const {
        return m_opponents.bosses().present() ? std::optional<BossView>(m_opponents.bosses().view())
                                              : std::nullopt;
    }
    const BossMeters& bossMeter() const { return m_opponents.meter(); }
    /** The wizard's visit once the boss has fallen. */
    const BossVictory& victory() const { return m_bossSequence.victory().state(); }
    /** The archive folder a character's figure was loaded from, for tests. */
    std::optional<std::filesystem::path> memberFigureDirectory(usize partyIndex) const {
        return partyIndex < m_players.size() && m_players[partyIndex].figure != nullptr
                   ? std::optional<std::filesystem::path>(m_players[partyIndex].figure->directory())
                   : std::nullopt;
    }
    const HelpMessages& help() const { return m_hud.help(); }
    const PartyHud& hud() const { return m_hud; }
    bool cinematicBars() const;
    /** Hands the selection's highlight to whichever owns the thing selected; null takes
     * it back. */
    void markSelection(const SelectionHighlight* highlight);
    bool hudVisible() const { return !m_sim.gameOver.active() && !cinematicBars(); }
    const MoveStrikes& strikes() const { return m_attacks.strikes(); }
    const AmbientDimmer& dimmer() const { return m_sim.dimmer; }
    /** Gives `player`'s character experience won in play, which also feeds its turbo meter
     * (unless it is in the middle of a turbo move), as a kill does in the original. */
    void awardExperience(s32 player, s32 amount, bool kill = true);
    /** The live record of `player`'s character, for the developer console to edit; null when
     * that player is not in. A level set there reaches the actor through restat(). */
    CharacterSave* liveSave(s32 player);
    /** The developer console presses a switch: on the next tick it goes off as if the whole
     * party stood on it, the camera following what it opens. False for one the party cannot
     * use, or none. */
    bool pressSwitch(usize index);
    /** The developer console opens a gate without a key, or shuts it again. */
    bool unlockGate(usize index);
    bool lockGate(usize index);
    /** The developer console makes a chest want a key, or open without one. */
    bool setChestLocked(usize index, bool locked);
    /** Gives `player`'s actor the pace its record's class and level allow now. */
    void restat(s32 player);
    /** Stands everyone standing at `position`, a pace apart, and the camera up over them,
     * for the developer console; false with nobody standing. */
    bool relocate(const Vec3& position);
    /** What the developer console grants a player. */
    void setCheats(s32 player, const ConsoleCheats& cheats);
    const ConsoleCheats& cheats(s32 player) const;
    /** The party size the level sets itself up for from now on, or none to follow the
     * players present: what is placed for a larger party appears, and the generators breed
     * for it, on the next tick. */
    void setPartyCountOverride(std::optional<s32> count);
    std::optional<s32> partyCountOverride() const { return m_sim.partyCountOverride; }
    /** The party size the level is set up for: the override, else `present`. */
    s32 partyCount(s32 present) const;
    const std::optional<ConsoleTravelRequest>& consoleTravelRequest() const {
        return m_sim.consoleTravel;
    }
    /** Draws no HUD: no names, status boxes, compass, messages or captions. */
    void setHudHidden(bool hidden) { m_hudHidden = hidden; }
    bool hudHidden() const { return m_hudHidden; }
    /** The seats this machine plays, a bit each: the companion arrows and the mechanics' HUD
     * show only theirs. Presentation only; every seat until told otherwise. */
    void setLocalSeats(u8 mask) { m_localSeats = mask; }
    /** Solo cameras, the netplay room's rule: each seat's own single-player camera, with
     * what any of them sees counting as on screen and nothing holding the party together.
     * The shared camera keeps following underneath for the hand-offs to the boss and
     * scripted cameras; the seats' cameras follow only under the rule, resting where the
     * level stood them up otherwise. Gameplay state, the same on every peer. */
    void setSoloCameras(bool solo) { m_sim.soloCameras = solo; }
    bool soloCameras() const { return m_sim.soloCameras; }
    /** An online match's rule: a fallen player is offered no Quit, since the party travels
     * whole to the match's end; leaving is the netplay menu's, as a drop. */
    void setFixedRoster(bool fixed) { m_sim.fixedRoster = fixed; }
    bool fixedRoster() const { return m_sim.fixedRoster; }
    /** Whether the seats look through their own cameras now: solo cameras, with neither a
     * boss camera nor an imported level's own in charge. */
    bool soloViewActive() const;
    /** The seat whose camera this machine looks through under solo cameras; below nought,
     * the lowest standing one. Presentation only. */
    void setViewSeat(s32 seat) { m_seats.setViewSeat(seat); }
    s32 viewSeat() const { return m_seats.viewSeat(); }
    const TowerCamera& seatCamera(s32 seat) const;
    const CompanionArrows& companionArrows() const { return m_arrows; }
    /** Looks through `camera` instead of the level's own, for the developer console's free
     * camera; nothing gives the level's back. Either change cuts rather than glides. */
    void setViewOverride(const std::optional<WorldCamera>& camera);
    /** Fells `player`'s character where it stands, for the developer console; false in the
     * tower, where nobody falls, or for one not standing. */
    bool kill(s32 player);
    /** Sends `player`'s character to wait in the tower at once, as a fallen one does
     * (unseen, untouchable, waited for by no portal, going on fallen, its keys left for the
     * others) but with no death or question: the seat of a peer gone from an online game.
     * False for a player not in. */
    bool waitInTower(s32 player);
    /** Flies `player`'s character by `offset`, through everything, facing `yaw` when
     * given; false for one not standing. */
    bool noclipMove(s32 player, const Vec3& offset, std::optional<f32> yaw);
    /** What stands under `cursor` (0 to 1 across and down the view as last rendered): the
     * nearest object, enemy, great one, generator, pickup, character or stood object the
     * ray from the eye meets; nothing with nothing there or nothing rendered yet. */
    std::optional<SceneSelection> pick(Vec2 cursor);
    /** The developer console's selection, marked out in the view while it holds. */
    void select(const std::optional<SceneSelection>& selection) { m_picking.select(selection); }
    const std::optional<SceneSelection>& selection() const { return m_picking.selection(); }
    /** Stands a copy of `level`'s object `name` at `position` facing `yaw`, loading that
     * level's archive if need be, for the developer console. */
    bool spawnObject(const LevelRef& level, std::string_view name, const Vec3& position, f32 yaw);
    usize spawnedObjectCount() const { return m_spawned.count(); }
    const SpawnedObjects& spawnedObjects() const { return m_spawned; }
    /** `level`'s item archive, loaded the first time and kept, for the bodies of the broods
     * the developer console brings in from it; null when it is not unpacked. */
    ItemArchive* borrowItems(const LevelRef& level);
    /** Lends the swarm `source`'s voices, its roster and the bank its `audio` record names,
     * for the kinds the developer console brings in from it; false without the bank. */
    bool borrowOpponentSounds(const LevelInfo& source, const LevelAudioInfo* audio);
    /** What `player`'s status box shows. */
    StatusBoxView status(s32 player) const { return PartyHud::status(player, m_players); }
    /** The turbo meter of `player`'s character, or null when that player is not in. */
    const TurboMeter* turboMeter(s32 player) const;
    /** `player`'s record in this level, or null when that player is not in. */
    const PlayerRuntime* runtime(s32 player) const {
        for (const PlayerRuntime& runtime : m_players) {
            if (runtime.actor.player() == player) {
                return &runtime;
            }
        }
        return nullptr;
    }
    /** Whether `player`'s character has fallen (dying or gone to the tower). */
    bool fallen(s32 player) const;
    /** Hurts `player`'s character, as anything in the level does. */
    void hurtPlayer(s32 player, f32 damage, HurtKind kind);
    /** A blast at `position`: hurts and breaks what is within `radius`. */
    void blast(const Vec3& position, f32 radius, f32 damage);
    const TransitionScreen& transition() const { return m_transition; }
    /** Whether the party has gone through a portal and the picture is coming up over it. */
    bool leaving() const { return m_sim.leaving; }
    bool canPause(s32 player) const;
    /** Whether `player`, not in the party, may join: in the tower, with nothing scripted
     * running (game_main's MG_PLAY, world 13, no options or scripted camera). */
    bool canJoin(s32 player) const;
    const GameOver& gameOver() const { return m_sim.gameOver; }
    /** Where the party is bound once update() has said Travel, and the realm it leaves. */
    const LevelRef& destination() const { return m_sim.destination; }
    bool secretTravel() const { return m_sim.secretTravel; }
    const SecretChallenge& challenge() const { return m_sim.challenge; }
    std::optional<ChallengeHud::Look> hourglassLook() const;
    /** Pause the parent stage while the party visits a secret challenge. */
    void suspendForChallenge();
    /** Return progress to the preserved stage without replacing its entry checkpoint. */
    void resumeFromChallenge(std::span<const PartyMember> party);
    /** The party as it stands, with all it has gathered, for the next level. */
    std::vector<PartyMember> party() const;
    /** `party` (a snapshot of this scene's) as it leaves a level it gives up: everyone as they
     * came in, keeping only what they were taught and the slots they are kept in (kill_player,
     * then PlayerRestoreState in the tower). */
    std::vector<PartyMember> abandonedParty(std::span<const PartyMember> party) const;
    /** Quitting the game plays the defeat caption and voice (MG_OVER), after which update
     * returns GameOver. */
    void startGameOver();
    /** Associates a manual save with the live participant without restarting the level. */
    void setSaveSlot(s32 player, std::optional<usize> slot);
    std::vector<LevelResults> levelResults() const;
    const EffectTrees& effects() const { return m_effects; }
    /** The powerup selector over `player`'s box. */
    const PowerupSelector& selector(s32 player) const { return m_hud.selector(player); }
    /** How large a character is drawn: an ogre, one grown by a powerup, one of level 99. */
    const SumnerHints& hintTexts() const { return m_sumnerVisit.texts(); }
    const PickupHud& pickups() const { return m_hud.pickups(); }
    const RuneMeter& runeMeter() const { return m_sim.runeMeter; }
    const AmbientSounds& ambience() const { return m_audio.ambience(); }
    /** How far Sumner's beam of light has come up, 0 to 1. */
    /** Whether the party is still materialising: held under the level's title until the start
     * camera has ridden in, or for the effect's life when there is no start camera. */
    bool spawning() const { return m_arrival.active(); }
    MissileStreak weaponStreak(const PlayerActor& actor) { return m_arsenal.weaponStreak(actor); }
    std::optional<f32> weaponTextureFrame(const PlayerFigure& figure) const {
        return m_arsenal.weaponTextureFrame(figure);
    }
    std::optional<PlayerArsenal::WeaponRider> weaponRider(const PlayerActor& actor,
                                                          PlayerFigure& figure) const {
        return m_arsenal.weaponRider(actor, figure);
    }
    const LevelArrivalPresentation& arrival() const { return m_arrival; }
    usize spawnEffectCount() const { return m_arrival.effectCount(); }
    /** The folder a character's figure came from, when it loaded. */
    std::optional<std::filesystem::path> seatFigureDirectory(s32 seat) const;
    /** Whether a character's figure carries its weapon. */
    bool weaponHeld(s32 player) const;
    /** The folder holding a character's figure at their level: the costume tier of ten
     * levels (`BLU00` for levels 1 to 9, `BLU10` for 10 to 19, and so on) when it is
     * unpacked, else the untiered costume. */
    static std::filesystem::path costumeDirectory(const std::filesystem::path& unpackedRoot,
                                                  const CharacterSave& save);
    const SumnerFigure& sumner() const { return m_sumner; }
    const TowerPromotion& promotion() const { return m_ceremonies.promotion(); }
    const TowerRelics& towerRelics() const { return m_ceremonies.relics(); }
    bool showingRelicCeremony() const { return relicCeremonyOn(); }
    s32 familiarTier(s32 player) const;

private:
    friend class ConsoleStage; // what the shared console verbs may change, within a tick
    u64 m_openGeneration = 0;
    Simulation m_sim;
    struct OutputFrame {
        u64 epoch = 0;
        u64 tick = 0;
        OutputLedger::Journal* journal = nullptr;
    };
    OutputFrame* m_outputFrame = nullptr; ///< call-scoped collector, never snapshot state
    void emitOutput(OutputPayload payload);
    PlayOutcome advanceNative(f64 deltaSeconds, const Inputs& inputs);
    /** One call's fixed clock: the ticks it covers and their seconds. */
    struct TickClock {
        s32 ticks = 0;
        f32 seconds = 0.0f;
    };
    /** The tick's phases, in the order advanceNative runs them: the preamble runs under every
     * hold (the presentation kept for interpolation, the clock, speech, music and the HUD's
     * clocks); a hold owns the whole tick while it lasts; play runs when nothing holds. */
    TickClock beginTick(f64 deltaSeconds);
    PlayOutcome advanceGameOver(s32 ticks, f32 seconds);
    void advanceScroll(s32 ticks, const Inputs& inputs);
    PlayOutcome advanceDeparture(s32 ticks, f32 seconds, const Inputs& inputs);
    void advanceHintScroll(s32 ticks, f32 seconds, const Inputs& inputs);
    void advanceEntrance(s32 ticks, f32 seconds, const Inputs& inputs);
    PlayOutcome advancePlay(s32 ticks, f32 seconds, const Inputs& inputs);
    /** Play's phases, in order: the powerups worn and the world stood in; the party's
     * movement, which gives the camera its subjects; the fixtures, hazards, opponents and
     * attacks, then the powerup clock; an imported level's own services (not while held); the
     * triggers; the portals (when they may end the tick); the cameras; the rune meter. */
    void advancePowerupsAndWorld(s32 ticks, f32 seconds, bool held);
    std::vector<CameraSubject> advancePartyMotion(s32 ticks, f32 seconds, const Inputs& inputs,
                                                  bool held);
    void advanceCombat(s32 ticks, f32 seconds, bool held);
    void advanceImportedLevel(f32 seconds, std::vector<CameraSubject>& subjects,
                              const Inputs& inputs);
    void applyImportedHazards(ImportedLevel& imported);
    void presentImportedPickups(ImportedLevel& imported);
    void advanceTriggers(f32 seconds);
    std::optional<PlayOutcome> advancePortals(s32 ticks, f32 seconds, const Inputs& inputs);
    void advanceCameras(f32 seconds, const std::vector<CameraSubject>& subjects);
    void advanceRuneMeter();
    /** Runs `action`'s shared verb for its seat at the start of a tick, in `play`'s game, and
     * says what came of it. */
    std::string applyConsoleAction(const ConsoleAction& action, ConsolePlay play);
    /** One tick of either game: `actions` for their seats in `play`'s game first, then the
     * tick itself, its outputs collected under `epoch` and `tick`. */
    RecordedStep advanceStep(u64 epoch, u64 tick, f64 deltaSeconds, const Inputs& inputs,
                             std::span<const ConsoleAction> actions, ConsolePlay play);
    void advanceConsoleFlight(f64 deltaSeconds, const Inputs& inputs);
    bool registerNativeDigestResources();
    bool appendSceneDigest(NativeStateDigest& digest, const LocalSnapshot* saved,
                           NativeDigestProfile* profile = nullptr) const;
    /** The scene's snapshot owners in digest order, each under its profile name with its reach
     * and its slot in the snapshot: saved, admitted, restored and hashed alike. */
    static const auto& sceneOwners();
    NativeStateResources m_nativeDigestResources; ///< immutable bindings, outside local history
    /** The bodies' shadows, after all of the level's floors (some of which are translucent). */
    PartyFigures::Scene figureScene(f32 frameBlend = 1.0f);
    void drawShadows(RenderDevice& device, const Mat4& clip, const Vec3& eye, f32 frameBlend,
                     f32 opponentBlend);
    bool scriptedCamera() const;
    /** The camera this machine looks through: the seat's own under solo cameras, else the
     * shared one. The previous is its last tick's, for the frame's interpolation. */
    WorldCamera presentedCamera() const;
    const std::optional<WorldCamera>& previousViewCamera() const;
    WorldCamera cameraOfSeat(s32 seat) const; ///< shaken like the shared one
    /** The camera `actor` plays against: its seat's own under solo cameras, else the shared. */
    WorldCamera cameraFor(const PlayerActor& actor) const;
    /** The yaw each member's stick is read against, `shared` unless solo cameras hold. */
    std::vector<f32> movementYaws(f32 shared) const;
    /** The seats whose companions the arrows point to: the shown seat alone under solo
     * cameras, else this machine's. */
    u8 arrowOwnSeats() const;
    using SeatCameraStates = SeatCameras::States;
    static_assert(SeatCameras::kSeats == static_cast<usize>(kPlayerCount));
    SeatCameraStates seatCameraStates() const;
    /** One subject per party member, in party order, moved by `displacement`. */
    std::vector<CameraSubject> partySubjects(const Vec3& displacement) const;
    /** The seats' cameras, each on its own standing character and a fallen one's with the
     * party, stood up, following for `seconds` or snapped to the party like the shared one. */
    void resetSeatCameras(std::span<const CameraSubject> subjects,
                          std::span<const WorldLocator> markers, const CameraRange& range);
    void followSeatCameras(std::span<const CameraSubject> subjects, f32 seconds);
    void snapSeatCameras(std::span<const CameraSubject> subjects);
    void watchOpponents(bool standPlaced = true);
    void gatherLights();
    void beginChallenge();
    void collectChallengeCoin(usize item);
    bool updateChallenge(f32 seconds);
    void spawnParty(std::span<const PartyMember> party, const PlayOptions& options);
    void throwWeapon(const PlayerActor& actor);
    u32 acceptedPlayers(const Inputs& inputs) const;
    std::vector<TriggerVisitor> visitors() const;
    /** Living floor occupants retain bridges even while a cinematic holds controls. */
    std::vector<s32> occupiedFloors() const;
    PartyPickups::Services pickupServices();
    /** The scene's answers to what its pickups put up: cards, counts, selectors and the
     * bosses' keys on its HUD. */
    class PickupRouting final : public PickupPresentation {
    public:
        explicit PickupRouting(PlayScene& scene) : m_scene(scene) {}
        void addCard(s32 seat, std::string_view texture) override;
        void showCount(s32 seat, std::string_view icon, s32 count, s32 total) override;
        void focusPickup(const PlayerActor& actor, s32 kind, u32 flags) override;
        void showRelics() override;

    private:
        PlayScene& m_scene;
    };
    void praiseStreaks(f32 seconds);
    /** Switches the level's mechanics as the settings say, every tick and at opening. */
    void syncMechanics();
    /** Lets the mechanics draw on the HUD for this machine's seats still in the game. */
    void drawMechanicsHud();
    bool leaveBy(usize portal);
    void updateFixtures(s32 ticks, f32 seconds);
    void updateTransporters(s32 ticks, f32 seconds, bool held);
    void updateSwitchCutscene(s32 ticks, f32 seconds);
    /** The scene's answers to what its fixtures report: harm to its members, lessons and cards
     * on its HUD, blasts and releases to its opponents, broken bottles to its attacks. */
    class FixtureRouting final : public FixtureHost {
    public:
        explicit FixtureRouting(PlayScene& scene) : m_scene(scene) {}
        void hurtMember(usize member, f32 damage, HurtKind kind, bool directed,
                        const PlayerImpact& impact) override;
        bool lesson(s32 id, usize member) override;
        void card(s32 seat, std::string_view name) override;
        void blastOpponents(const Vec3& position, f32 radius, f32 damage, std::vector<s32>& reached,
                            u32 flags) override;
        bool releaseEnemy(s32 record, const Vec3& position, s32 count) override;
        void shatterPotion(s32 kind, const Vec3& position) override;

    private:
        PlayScene& m_scene;
    };
    /** The scene's answers to what its attacks report: one player's blow on another hurts that
     * member as a blow from somewhere, and a melee hit rumbles its attacker's controller. */
    class AttackRouting final : public AttackHost {
    public:
        explicit AttackRouting(PlayScene& scene) : m_scene(scene) {}
        void hurtByAttack(usize member, f32 damage, HurtKind kind,
                          const PlayerImpact& impact) override;
        void meleeHit(s32 seat) override;

    private:
        PlayScene& m_scene;
    };
    /** The scene's answers to what its opponents report; one is made for each update or
     * settling. */
    class OpponentRouting;
    /** The scene's answers to what a boss encounter asks; one is made for each update or
     * settling. */
    class BossRouting;
    /** Runs the opponents for the tick against the fixtures, answered by the scene; an imported
     * level's arena gives the cover, otherwise the safe rocks do. */
    void advanceOpponents(s32 ticks, f32 seconds);
    /** Settles what the opponents still owe the party before it leaves the level. */
    void settleOpponentRewards();
    PlayerAttacks::Targets attackTargets();
    void answerTowerPrompts(const Inputs& inputs);
    void hurt(usize index, f32 damage, HurtKind kind, HurtSource source,
              const PlayerImpact& impact = {}, bool armorAbsorbed = false);
    /** What a blow to `runtime` (the player at `index`) sounds, shows and teaches. */
    PlayerHealth::Events healthEvents(usize index, PlayerRuntime& runtime);
    void updateEnemies(s32 ticks, f32 seconds);
    void strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer);
    void strikeGenerator(s32 id, f32 power, s32 byPlayer);
    void strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                       std::optional<Vec3> where = std::nullopt, bool close = false);
    void updateVictory(s32 ticks, f32 seconds);
    void settleBlasts();
    bool postHelp(s32 id, usize index, s32 number = -1);
    /** Answers levels gained with immediate health, effects and narration.
     * Appearance upgrades and title ceremonies wait for a tower return. */
    void updateLevels();
    void updatePromotion(s32 ticks, f32 seconds);
    void updateTowerRelics(s32 ticks, f32 seconds);
    bool relicCeremonyOn() const;
    static constexpr f32 kLevelUpHealth = 100.0f;
    void sayWithName(usize index, std::string_view line, f32 wait);
    void updateHazardSurfaces(f32 seconds);
    void launchWeapon(usize index, const Vec3& direction, f32 scale, bool spreads);
    /** The release-time target of a member looking along `direction`, or none with the
     * Auto Aim setting off. */
    std::optional<Vec3> assistedAim(usize index, const Vec3& direction);
    usize standingCount() const;
    bool isDown(usize index) const {
        return index < m_players.size() && m_players[index].life != PlayerLife::Standing;
    }
    void updateAmbience();
    void gatherRecordedAudio(const Inputs& inputs, PlayOutcome outcome);
    void beginSpawn(RenderDevice& device, const std::optional<WorldCamera>& marker);
    bool anyButton(const Inputs& inputs) const;
    bool openMessage(std::string_view name, usize page);
    void handleTriggerEvents();
    /** The scene's answers to what the party's movement asks as it steps; one is made for
     * each step. */
    class MotionRouting;
    /** Moves the party for the tick, each member's stick read against its yaw in `yaws`. */
    std::vector<CameraSubject> stepParty(std::span<const PlayInput> inputs, bool held,
                                         std::span<const f32> yaws, s32 ticks, f32 seconds);
    ScenePicking::Scene pickingScene();
    TowerCeremonies::Scene ceremonyScene();
    void perform(usize index, PartyMotion::Action action);
    Vec3 limitStep(usize index, const Vec3& before, const Vec3& after) const;
    void updateSumnerVisit(f32 seconds);
    void updateHints(const Inputs& inputs, s32 ticks);
    void openImportedDialogue();
    bool importedOpening() const;
    void updateImportedDialogue(const Inputs& inputs, s32 ticks);

    RenderDevice* m_device = nullptr;
    const GameServices* m_services = &GameServices::none(); ///< the game's, from open on
    Links m_links;
    LevelWorld* m_world = nullptr;
    CompassHud m_compass;
    ClassDataSet m_classes;
    PartyHud m_hud;
    PickupRouting m_pickupRouting{*this}; ///< what the pickups put up
    Canvas m_canvas;
    TowerCamera m_camera;
    SeatCameras m_seats; ///< the seats' own cameras, under solo cameras
    std::optional<Mat4> m_presentedClip;
    WorldCamera m_presentedView;
    f32 m_presentedWorldAlpha = -1;
    std::optional<Mat4> m_presentedCanvas; ///< local pointer mapping, never simulation state
    std::optional<WorldCamera> m_previousCamera;
    std::optional<WorldCamera> m_previousSeatView; ///< this machine's view alone, never digested
    std::optional<WorldCamera> m_viewOverride;
    ScenePicking m_picking;
    std::vector<PlayerRuntime> m_players;
    LevelSoundscape m_audio;
    SceneSoundEvents m_recordedAudio;
    ChallengeHud m_challengeHud;
    MechanicsFacade m_mechanics; ///< this level's instance of every mechanic the build carries
    bool m_previousBossCamera = false;
    bool m_hudHidden = false;
    u8 m_localSeats = 0xF;
    bool m_open = false;
    const Texture* m_glowSheet = nullptr; ///< FONT32's glow, borrowed from the static set
    const Texture* m_runeFrame = nullptr;
    const Texture* m_runeColumn = nullptr;
    SumnerFigure m_sumner;
    TextureSet m_staticTextures;
    LevelMessages m_messages;
    PlayerArsenal m_arsenal;
    ExitPortals m_portals;
    LevelTransporters m_transporters;
    PlayerAttacks m_attacks;
    AttackRouting m_attackRouting{*this}; ///< what the attacks report into
    LevelFixtures m_fixtures;
    FixtureRouting m_fixtureRouting{*this}; ///< what the fixtures report into
    LevelOpponents m_opponents;
    SpawnedObjects m_spawned; ///< world objects the developer console stood
    std::vector<std::pair<std::string, std::unique_ptr<LevelItemArchives>>>
        m_borrowedItems; ///< other levels' item archives, by level, for the console
    TransitionScreen m_transition;
    EffectTrees m_effects;
    std::vector<PointLight> m_lights; ///< this frame's, gathered before it is drawn
    BossSequence m_bossSequence;
    SumnerVisit m_sumnerVisit;
    TowerCeremonies m_ceremonies;
    TriggerCues m_triggerCues;

    ItemArchive m_weapons;              ///< shared weapon and effect assets
    LevelArrivalPresentation m_arrival; ///< borrows the weapons archive
    PortalDeparture m_departure;
    PartyFigures m_figures;
    PartyNames m_names;
    CompanionArrows m_arrows; ///< presentation only
};

} // namespace gdl::game
