#pragma once

#include "expansions/dreamcast/audio/LegendsDreamcastAmbient.h"
#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"
#include "expansions/dreamcast/audio/LegendsDreamcastSpeech.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBossAssets.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBossVisit.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBosses.h"
#include "expansions/dreamcast/camera/LegendsDreamcastArenaCamera.h"
#include "expansions/dreamcast/camera/LegendsDreamcastCamera.h"
#include "expansions/n64/audio/LegendsAmbient.h"
#include "expansions/n64/audio/LegendsAnnouncements.h"
#include "expansions/n64/audio/LegendsSounds.h"
#include "expansions/n64/audio/LegendsSpeech.h"
#include "expansions/n64/bosses/LegendsBosses.h"
#include "expansions/n64/enemies/LegendsEnemySounds.h"
#include "expansions/n64/level/LegendsAdvice.h"
#include "expansions/n64/level/LegendsBlast.h"
#include "expansions/n64/level/LegendsSumner.h"
#include "expansions/shared/scenes/LegendsPreview.h"
#include "game/bosses/LegendProfile.h"
#include "game/level/ImportedLevel.h"
#include "game/widgets/DialoguePresentation.h"

namespace gdl::game {

/** Native map resources hosted by PlayScene, not a second player implementation. */
class LegendsLevel final : public ImportedLevel {
public:
    LegendsLevel(RenderDevice& device, const formats::LegendsMap& map,
                 std::unique_ptr<LegendsSceneAdapter> adapter,
                 const std::filesystem::path& directory, const std::filesystem::path& characterRoot,
                 std::shared_ptr<LegendsAdviceState> advice = {});
    void capturePresentation() override;
    void servicePreservedSpeech(f32 seconds) override;
    void bindStrings(const StringTable* strings) override {
        m_strings = strings;
        m_dreamcastVisit.bindStrings(strings);
    }
    const LegendsDreamcastBossCeremony& dreamcastBossCeremony() const {
        return m_dreamcastVisit.state();
    }
    const LegendsDreamcastBossVisit& dreamcastBossVisit() const { return m_dreamcastVisit; }
    const DialoguePresentation* messagePresentation() const override {
        return m_messageStyle.get();
    }
    void drawEntranceTitle(Canvas& canvas, f32 width) const override;
    void update(f32 seconds, bool timeStopped) override;
    void bindCollision(WorldCollision& collision) const override;
    void syncCollision(WorldCollision& collision) const override;
    std::span<const u32> surfaceHazardFlags() const override {
        return adapter().surfaceHazardFlags();
    }
    std::optional<ImportedWorldObject> object(s32 id) const override { return m_world.object(id); }
    std::optional<ImportedObjectHit> pickObject(const Vec3& origin, const Vec3& direction,
                                                const WorldCamera& view, f32 alpha) const override {
        return m_world.pickObject(origin, direction, view, alpha);
    }
    void highlightObject(RenderDevice& device, const Mat4& clip, const WorldCamera& view, f32 alpha,
                         s32 id, const SelectionHighlight& highlight) const override {
        m_world.highlightObject(device, clip, view, alpha, id, highlight);
    }
    void draw(RenderDevice& device, const Mat4& clip, const WorldCamera& view, TreeModel::Pass pass,
              f32 alpha) const override;
    void resetCamera(std::span<const CameraSubject> subjects) override;
    void trackBoss(std::optional<Vec3> floorAnchor) override;
    void trackBoss(const BossCameraSubject& subject) override;
    void followCamera(std::span<const CameraSubject> subjects, f32 seconds) override;
    std::optional<WorldCamera> camera(f32 alpha = -1) const override;
    std::optional<Vec3> cameraAttention() const override;
    f32 horizontalFov(f32 preferred) const override;
    bool playerLevelGrowth() const override { return false; }
    s32 healthLimit() const override;
    bool opponentsReady() const override;
    bool protectsParty() const override { return m_dreamcastBoss && m_bossDefeated; }
    Vec3 resolveMovement(const Vec3& from, const Vec3& to, f32 radius, f32 height) const override;
    void touchScenery(s32 player, const Vec3& from, const Vec3& to, f32 radius,
                      f32 height) override;
    void interactionButtons(u32 pressedSeats) override { m_interactionButtons = pressedSeats; }
    std::optional<ImportedDialogue> takeDialogue() override;
    std::vector<std::string> answerDialogue(s32 topic) override;
    void dismissDialogue() override;
    bool takeShopRequest() override;
    bool placeOpeningParty(std::span<PlayerActor*> party) override;
    void holdParty(std::span<PlayerActor* const> party) override;
    void updateParty(std::span<PlayerActor*> players, f32 seconds, bool activatePowerups) override;
    void initializeParty(std::span<const PartyMember> party);
    void announceProgress(std::span<const PartyMember> before, std::span<const PartyMember> after);
    /** Only the app's completed native departure calls this; console jumps do not. */
    void completeTravel(std::span<PartyMember> party) const;
    std::vector<ImportedPickup> takePickups() override;
    std::vector<ImportedTarget> targets() const override;
    bool strike(s32 id, f32 power, u32 flags, s32 player) override;
    bool strikeWorldObject(s32 object, f32 power, u32 flags, s32 player) override;
    std::vector<Obstacle> obstacles() const override;
    bool travelRequested() const override { return m_destination.has_value(); }
    std::optional<ImportedDeparture> departure(f32 alpha = -1) const override;
    std::vector<PointLight> pointLights() const override;
    std::span<const ImportedGenerator> importedGenerators() const override;
    std::span<const ImportedActor> importedActors() const override {
        return m_world.adapter()->actors();
    }
    void releaseEnemies(const EnemyRelease& release) override {
        m_world.adapter()->releaseDeaths(release);
    }
    ItemArchive* importedActorArchive(s32 kind) const override {
        return m_world.adapter()->actorArchive(kind);
    }
    const ImportedBoss* importedBoss() const override {
        if (m_dreamcastBoss) {
            return &m_dreamcastBoss->combat;
        }
        return m_boss ? &m_boss->combat : nullptr;
    }
    bool hasBossLegend(const CharacterSave& save) const override;
    bool consumeBossLegend(CharacterSave& save) override;
    bool legendRiteRestrainsParty() const override;
    bool playBossLegendSound(LegendSound phase) override;
    ItemArchive* bossLegendArchive() const override { return m_bossLegend.get(); }
    const LegendProfile* bossLegendProfile() const override {
        return m_bossLegend ? &m_bossLegendProfile : nullptr;
    }
    bool hasEnemyAssets() const override {
        return platform() == LegendsPlatform::N64 || platform() == LegendsPlatform::Dreamcast;
    }
    std::unique_ptr<ItemArchive> loadEnemyAssets(s32 kind) const override;
    std::unique_ptr<CombatantAssets>
    loadBossAssets(RenderDevice& device, const std::filesystem::path& commonRoot) const override;
    void bossIntro(const Vec3& position) override;
    bool bossActivationReady(const Vec3& position) const override;
    void bossArenaCue(const Vec3& position) override;
    /** An N64 boss's rewards fly as its death has played, about the boss as it then stands. */
    void bossDeathPlayed(const Vec3& position) override;
    void bossDefeated(const Vec3& position, std::span<PlayerActor*> players) override;
    std::vector<Mat4> arenaAnchors() const override { return adapter().arenaAnchors(); }
    std::vector<CombatArenaTarget> arenaTargets() const override {
        return adapter().arenaTargets();
    }
    void activateArena(const CombatArenaActivation& activation) override {
        m_world.adapter()->scheduleArenaActivation(activation.index, activation.delay);
    }
    bool arenaBlocksSegment(const Vec3& from, const Vec3& to, f32 radius) const override {
        return adapter().arenaBlocksSegment(from, to, radius);
    }
    std::vector<MissileStop> arenaMissileStops() const override {
        return adapter().arenaMissileStops();
    }
    void updateGenerators(std::span<const ImportedGeneratorState> states) override;
    void supportOpponents(std::span<const s32> floorOwners) override;
    void watch(const ViewVolume& view) override { m_world.adapter()->setTrapView(view); }
    std::vector<ImportedHazardHit> takeHazardHits() override;
    void affectArea(const ImportedAreaDamage& apply) override;
    bool playOpponentSound(std::string_view name, const Vec3& position) override;
    bool playEnemyFeedback(const EnemyFeedback& feedback, f32 health) override;
    bool playEnemyCue(const EnemyCue& cue) override;
    bool playEnemyMelee(s32 kind, s32 tier) override;
    std::optional<bool> suicideUsesGas() const override;
    bool playDeathCue(const DeathEvent& cue) override;
    bool setDeathContact(bool active) override;
    bool playGeneratorFeedback(const GeneratorEvent& event) override;
    std::optional<SoundSequence>
    gameplayAnnouncement(std::span<const std::string_view> lines,
                         std::optional<AnnouncementSubject> subject) const override {
        return m_announcements.resolve(lines, subject);
    }
    void playSounds(SoundPlayer& output, const Vec3& listener) override;
    std::optional<MusicDuck> ambientMusicDuck() const override { return m_ambientMusicDuck; }
    void pauseSounds() override;
    void stopSounds() override;
    const std::optional<std::string>& destination() const { return m_destination; }
    bool cancelDeparture() override;
    std::optional<u32> exitReminder() const { return m_exitReminder; }
    /** A committed native transition keeps the shared mixer's phrase alive. */
    void continueSpeechFrom(LegendsLevel& previous);
    const std::filesystem::path& directory() const { return m_directory; }
    LegendsPlatform platform() const { return m_world.adapter()->platform(); }
    const LegendsSceneAdapter& adapter() const { return *m_world.adapter(); }

private:
    std::unique_ptr<CombatantAssets>
    loadN64BossAssets(RenderDevice& device, const std::filesystem::path& commonRoot) const;
    void n64BossDefeated(const Vec3& position, std::span<PlayerActor*> players);
    void dreamcastBossDefeated(const Vec3& position, std::span<PlayerActor*> players);
    bool updateSumner(std::span<PlayerActor*> players, f32 seconds, bool blocked);
    bool updateN64Sumner(std::span<PlayerActor*> players, f32 seconds, bool blocked);
    bool updateDreamcastSumner(std::span<PlayerActor*> players, f32 seconds, bool blocked);
    /** A Tower ceremony waiting to open the visit stands the party facing Sumner, keeping each
     * member's arrival heading for its hand-back and their mean feet for its start. */
    bool placeN64OpeningParty(std::span<PlayerActor*> party);
    /** A Tower ceremony turns the party it holds toward each shot's focus, and its hand-back
     * turns each member back to the heading it stood at before the cut. */
    void holdN64Party(std::span<PlayerActor* const> party);
    std::vector<std::string> answerN64Dialogue(s32 topic);
    std::unique_ptr<ItemArchive> loadN64EnemyAssets(s32 kind) const;
    std::unique_ptr<ItemArchive> loadDreamcastEnemyAssets(s32 kind) const;
    bool playN64OpponentSound(std::string_view name);
    bool playDreamcastOpponentSound(std::string_view name);
    bool playN64EnemyFeedback(const EnemyFeedback& feedback, f32 health);
    bool playDreamcastEnemyFeedback(const EnemyFeedback& feedback);
    bool playN64EnemyCue(const EnemyCue& cue);
    bool playDreamcastEnemyCue(const EnemyCue& cue);
    bool playN64EnemyMelee(s32 kind, s32 tier);
    bool playDreamcastEnemyMelee(s32 kind, s32 tier);
    bool playN64GeneratorFeedback(const GeneratorEvent& event);
    bool playDreamcastGeneratorFeedback(const GeneratorEvent& event);
    void continueN64SpeechFrom(LegendsLevel& previous);
    /** Takes over a Dreamcast phrase still playing from the previous level, if any. */
    bool continueDreamcastSpeechFrom(LegendsLevel& previous);
    void updateExitReminder(std::span<const CharacterSave* const> party);
    LegendsPreview m_world;
    std::unique_ptr<DialoguePresentation> m_messageStyle;
    const StringTable* m_strings = nullptr; ///< borrowed from the hosting scene's services
    f64 m_entranceSeconds = 0;
    std::filesystem::path m_directory;
    std::optional<LegendsCamera> m_camera;
    std::optional<LegendsDreamcastLevelCamera> m_dreamcastCamera;
    std::optional<LegendsDreamcastTowerCamera> m_dreamcastTowerCamera;
    std::optional<LegendsDreamcastArenaCamera> m_dreamcastArenaCamera;
    std::optional<BossCameraInfo> m_dreamcastArenaConfig;
    BossCameraSubject m_dreamcastBossSubject;
    CameraRange m_dreamcastRange;
    std::optional<LegendsSumner> m_sumner;
    std::optional<LegendsDreamcastSumner> m_dreamcastSumner;
    std::array<bool, 4> m_podiumContacts{};
    /** e086007c's mean feet of the standing party a Tower ceremony visit opened with, which
     * the ceremony's start takes. */
    std::optional<Vec3> m_ceremonyParty;
    std::vector<LegendsProgress> m_hintProgress;
    std::optional<ImportedDialogue> m_dialogue;
    bool m_dialogueOpen = false;
    u32 m_interactionButtons = 0;
    bool m_shopRequested = false;
    u32 m_historyMilestone = 14;
    f32 m_cameraTickSeconds = 1.0f / 60.0f;
    std::optional<std::string> m_destination;
    bool m_departureFailed = false;
    f64 m_departureSeconds = 0;
    f64 m_previousDepartureSeconds = 0;
    std::unique_ptr<Texture> m_departureSkin;
    ClassDataSet m_classes;
    std::vector<ImportedPickup> m_pickups;
    LegendsSounds m_sounds;
    LegendsDreamcastSounds m_dreamcastSounds;
    SoundHandle playNativeSound(SoundPlayer& output, u32 token, f32 volume = 1, bool loop = false);
    std::optional<LegendsEnemySounds> m_enemySounds;
    std::vector<LevelEnemy> m_dreamcastEnemyRoster;
    s32 m_dreamcastRealm = -1;
    LegendsSpeech m_speech;
    LegendsDreamcastSpeech m_dreamcastSpeech;
    std::vector<u32> m_dreamcastRuneAnnouncements;
    LegendsAnnouncements m_announcements;
    std::shared_ptr<LegendsAdviceState> m_advice;
    bool m_deathAdviceSpeechPending = false;
    LegendsAmbient m_ambient;
    LegendsDreamcastAmbient m_dreamcastAmbient;
    struct AmbientSource {
        u32 nativeIndex = 0;
        usize owner = 0;
        Vec3 position{0};
    };
    std::vector<AmbientSource> m_dreamcastAmbientSources;
    SoundHandle m_dreamcastAmbientHandle = kNoSound;
    std::optional<u32> m_dreamcastAmbientIndex;
    std::optional<MusicDuck> m_ambientMusicDuck;
    bool m_entrySpeechPending = true;
    std::optional<u32> m_exitReminder;
    bool m_exitSpeechPlayed = false;
    bool m_entrySpeechAwaitQueue = false;
    bool m_cameraReady = false;
    SoundPlayer* m_soundOutput = nullptr;
    std::vector<SoundHandle> m_soundHandles;
    std::vector<u32> m_opponentSounds;
    SoundHandle m_deathDrainSound = kNoSound;
    bool m_deathDrainContact = false;
    std::vector<std::string> m_missingOpponentSounds;
    struct MotionSound {
        u32 id = 0;
        SoundHandle handle = kNoSound;
    };
    std::vector<MotionSound> m_motionSounds;
    std::vector<MotionSound> m_ambientSounds;
    std::vector<LegendsBlast> m_blasts;
    std::vector<ImportedHazardHit> m_hazardHits;
    std::array<f32, 4> m_trapArmor{};
    std::vector<LegendsPortalVisitor> m_visitors;
    f32 m_trapDamageScale = 1;
    std::optional<LegendsBossEncounter> m_boss;
    std::optional<LegendsDreamcastBossEncounter> m_dreamcastBoss;
    std::optional<LegendsDreamcastBossCamera> m_dreamcastBossCamera;
    LegendsDreamcastBossVisit m_dreamcastVisit;
    std::unique_ptr<ItemArchive> m_bossLegend;
    LegendProfile m_bossLegendProfile;
    std::optional<LegendsBossCamera> m_bossCamera;
    bool m_bossDefeated = false;
    f64 m_bossDepartureSeconds = 0;
    Vec3 m_playerStart{0};
    mutable WorldCollision* m_collision = nullptr; ///< borrowed from the hosting LevelWorld
};

} // namespace gdl::game
