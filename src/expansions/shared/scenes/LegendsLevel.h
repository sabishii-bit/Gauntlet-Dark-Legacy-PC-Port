#pragma once

#include "expansions/n64/audio/LegendsAmbient.h"
#include "expansions/n64/audio/LegendsAnnouncements.h"
#include "expansions/n64/audio/LegendsSounds.h"
#include "expansions/n64/audio/LegendsSpeech.h"
#include "expansions/n64/bosses/LegendsBosses.h"
#include "expansions/n64/enemies/LegendsEnemySounds.h"
#include "expansions/n64/level/LegendsAdvice.h"
#include "expansions/n64/level/LegendsBlast.h"
#include "expansions/n64/level/LegendsSumner.h"
#include "expansions/n64/menu/LegendsDialogue.h"
#include "expansions/shared/scenes/LegendsPreview.h"
#include "game/bosses/LegendProfile.h"
#include "game/level/ImportedLevel.h"

namespace gdl::game {

/** Native map resources hosted by PlayScene, not a second player implementation. */
class LegendsLevel final : public ImportedLevel {
public:
    LegendsLevel(RenderDevice& device, const formats::LegendsMap& map,
                 std::unique_ptr<LegendsSceneAdapter> adapter,
                 const std::filesystem::path& directory, const std::filesystem::path& characterRoot,
                 std::shared_ptr<LegendsAdviceState> advice = {});
    void capturePresentation() override;
    const DialoguePresentation* messagePresentation() const override {
        return m_messageStyle.get();
    }
    void update(f32 seconds, bool timeStopped) override;
    void bindCollision(WorldCollision& collision) const override;
    void syncCollision(WorldCollision& collision) const override;
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
    void followCamera(std::span<const CameraSubject> subjects, f32 seconds) override;
    std::optional<WorldCamera> camera(f32 alpha = -1) const override;
    std::optional<Vec3> cameraAttention() const override;
    bool opponentsReady() const override;
    Vec3 resolveMovement(const Vec3& from, const Vec3& to, f32 radius, f32 height) const override;
    void touchScenery(s32 player, const Vec3& from, const Vec3& to, f32 radius,
                      f32 height) override;
    std::optional<ImportedDialogue> takeDialogue() override;
    std::vector<std::string> answerDialogue(s32 topic) override;
    void dismissDialogue() override;
    bool takeShopRequest() override;
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
    const ImportedBoss* importedBoss() const override { return m_boss ? &m_boss->combat : nullptr; }
    bool hasBossLegend(const CharacterSave& save) const override;
    bool consumeBossLegend(CharacterSave& save) override;
    bool playBossLegendSound(LegendSound phase) override;
    ItemArchive* bossLegendArchive() const override { return m_bossLegend.get(); }
    const LegendProfile* bossLegendProfile() const override {
        return m_bossLegend ? &m_bossLegendProfile : nullptr;
    }
    bool hasEnemyAssets() const override { return platform() == LegendsPlatform::N64; }
    std::unique_ptr<ItemArchive> loadEnemyAssets(s32 kind) const override;
    std::unique_ptr<CombatantAssets>
    loadBossAssets(RenderDevice& device, const std::filesystem::path& commonRoot) const override;
    void bossIntro(const Vec3& position) override;
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
    bool updateSumner(std::span<PlayerActor*> players, f32 seconds, bool blocked);
    void updateExitReminder(std::span<const CharacterSave* const> party);
    LegendsPreview m_world;
    std::unique_ptr<LegendsDialogue> m_messageStyle;
    std::filesystem::path m_directory;
    std::optional<LegendsCamera> m_camera;
    std::optional<TowerCamera> m_dreamcastCamera;
    CameraRange m_dreamcastRange;
    WorldCamera m_previousDreamcastCamera;
    std::optional<LegendsSumner> m_sumner;
    std::array<bool, 4> m_podiumContacts{};
    std::vector<LegendsProgress> m_hintProgress;
    std::optional<ImportedDialogue> m_dialogue;
    bool m_dialogueOpen = false;
    bool m_shopRequested = false;
    u32 m_historyMilestone = 14;
    std::vector<std::string> m_milestonePages;
    f32 m_cameraTickSeconds = 1.0f / 60.0f;
    std::optional<std::string> m_destination;
    bool m_departureFailed = false;
    f64 m_departureSeconds = 0;
    f64 m_previousDepartureSeconds = 0;
    std::unique_ptr<Texture> m_departureSkin;
    ClassDataSet m_classes;
    std::vector<ImportedPickup> m_pickups;
    LegendsSounds m_sounds;
    std::optional<LegendsEnemySounds> m_enemySounds;
    LegendsSpeech m_speech;
    LegendsAnnouncements m_announcements;
    std::shared_ptr<LegendsAdviceState> m_advice;
    bool m_deathAdviceSpeechPending = false;
    LegendsAmbient m_ambient;
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
    std::unique_ptr<ItemArchive> m_bossLegend;
    LegendProfile m_bossLegendProfile;
    std::optional<LegendsBossCamera> m_bossCamera;
    bool m_bossDefeated = false;
    f64 m_bossDepartureSeconds = 0;
    Vec3 m_playerStart{0};
    mutable WorldCollision* m_collision = nullptr; ///< borrowed from the hosting LevelWorld
};

} // namespace gdl::game
