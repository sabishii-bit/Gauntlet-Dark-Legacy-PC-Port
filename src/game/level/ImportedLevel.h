#pragma once

#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/ui/Canvas.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"

#include "game/audio/AnnouncementSubject.h"
#include "game/audio/LegendSound.h"
#include "game/camera/BossCamera.h"
#include "game/camera/TowerCamera.h"
#include "game/combat/CombatArenaTarget.h"
#include "game/critters/CombatEvents.h"
#include "game/critters/CombatantAssets.h"
#include "game/level/ImportedActors.h"
#include "game/level/ImportedBoss.h"
#include "game/level/ImportedGenerators.h"
#include "game/level/ImportedHazards.h"
#include "game/level/ImportedTargets.h"
#include "game/level/ImportedWorldObject.h"
#include "game/level/ItemFigure.h"
#include "game/level/MissileStop.h"
#include "game/players/ItemPickup.h"

namespace gdl {
class StringTable;
}

namespace gdl::game {
class PlayerActor;
struct CharacterSave;
struct LegendProfile;
struct GeneratorEvent;
class DialoguePresentation;

/** Feedback for an already completed native pickup, never a second inventory grant.
 * Player identifies the input/HUD slot, not an index into the living visitors.
 * Native campaign awards leave offer empty rather than borrowing GDL relic kinds. */
struct ImportedPickup {
    s32 player = 0;
    ItemOffer offer;
    std::string name;
    ItemTaking taking;
    std::vector<std::string> scrollPages;
    bool scrollKeys = false; ///< explicit localization keys rather than authored document text
};

/** Presentation about each departing player's existing feet/orientation.
 * Optional native overrides leave ordinary Dark Legacy departures unchanged. */
struct ImportedDeparture {
    f32 yaw = 0;
    f32 scale = 1;
    const Texture* skin = nullptr; ///< owned by the imported level, preloaded before play
    bool finished = false;
    /** A native white-screen departure replaces the normal transition picture.
     * Null retains the usual post-departure black transition. */
    std::optional<f32> whiteFade;
    bool keepVisible = false; ///< stationary special exits retain the party until covered
    bool transportSound = true;
    std::optional<SceneFog> fog;
    TreeModel::SkinAlpha skinAlpha = TreeModel::SkinAlpha::PreserveCoverage;

    bool covered(bool transitionCovered) const {
        return finished &&
               (whiteFade ? std::isfinite(*whiteFade) && *whiteFade >= 1 : transitionCovered);
    }
};

/** Localized topic IDs supplied by a native resident, rendered by the shared menu. */
struct ImportedDialogue {
    struct Topic {
        s32 code = 0;
        std::string label;
    };
    s32 player = 0;
    std::string title;
    std::vector<Topic> topics;
};

/** Native level presentation and scenery rules hosted by the ordinary gameplay scene.
 * Character controls, combat, inventory, HUD and pause remain PlayScene responsibilities.
 * Implementations retain their native archive resources for as long as LevelWorld uses them.
 * Imported levels are currently offline; this interface is not a rollback state contract. */
class ImportedLevel {
public:
    virtual ~ImportedLevel() = default;
    /** Advance only a retained departure phrase while results/shop owns the
     * screen. Never resumes the old map's gameplay or ambient loops. */
    virtual void servicePreservedSpeech(f32 /*seconds*/) {}
    virtual void capturePresentation() = 0;
    /** Borrow the application's localized text for native scripted presentations. */
    virtual void bindStrings(const StringTable* /*strings*/) {}
    virtual const DialoguePresentation* messagePresentation() const { return nullptr; }
    /** Optional native entry title; resources and its clock belong to the adapter. */
    virtual void drawEntranceTitle(Canvas& /*canvas*/, f32 /*width*/) const {}
    virtual std::vector<PointLight> pointLights() const { return {}; }
    virtual void update(f32 seconds, bool timeStopped) = 0;
    virtual void draw(RenderDevice& device, const Mat4& clip, const WorldCamera& camera,
                      TreeModel::Pass pass, f32 presentationAlpha) const = 0;
    /** Mark native moving surface owners once, then synchronize their tick poses
     * before the common player/enemy floor-riding phase. */
    virtual void bindCollision(WorldCollision& /*collision*/) const {}
    virtual void syncCollision(WorldCollision& /*collision*/) const {}
    /** Common HazardSurfaces flags, indexed by native collision owner. The
     * adapter resolves its own flag encoding and hierarchy before exposing them.
     * Empty retains the ordinary layout's hazard metadata. */
    virtual std::span<const u32> surfaceHazardFlags() const { return {}; }

    virtual std::optional<ImportedWorldObject> object(s32 /*id*/) const { return {}; }
    virtual std::optional<ImportedObjectHit> pickObject(const Vec3& /*origin*/,
                                                        const Vec3& /*direction*/,
                                                        const WorldCamera& /*view*/,
                                                        f32 /*alpha*/) const {
        return {};
    }
    virtual void highlightObject(RenderDevice& /*device*/, const Mat4& /*clip*/,
                                 const WorldCamera& /*view*/, f32 /*alpha*/, s32 /*id*/,
                                 const SelectionHighlight& /*highlight*/) const {}

    virtual void resetCamera(std::span<const CameraSubject> /*subjects*/) {}
    /** Live boss floor anchor once the encounter has begun; null removes a defeated boss.
     * The native adapter owns its camera focus offset and fitting radius. */
    virtual void trackBoss(std::optional<Vec3> /*floorAnchor*/) {}
    /** Native arena cameras may also need live facing, size and activation state.
     * Floor-anchor-only adapters follow the boss once the encounter has begun. */
    virtual void trackBoss(const BossCameraSubject& subject) {
        if (subject.engaged) {
            trackBoss(std::optional<Vec3>{subject.position});
        }
    }
    virtual void followCamera(std::span<const CameraSubject> /*subjects*/, f32 /*seconds*/) {}
    virtual std::optional<WorldCamera> camera(f32 /*alpha*/ = -1.0f) const { return std::nullopt; }
    virtual std::optional<Vec3> cameraAttention() const { return std::nullopt; }
    /** A native camera can require a wider lens without changing saved video settings. */
    virtual f32 horizontalFov(f32 preferred) const { return preferred; }
    /** Imported worlds may keep the character's initial physical artwork size
     * without changing its stats, costume tier or growth powerups. */
    virtual bool playerLevelGrowth() const { return true; }
    /** Native opening cameras may hold both placed enemies and generator births.
     * Ordinary imported maps without a scripted opening are ready immediately. */
    virtual bool opponentsReady() const { return true; }
    /** Some native victory scripts protect the party without locking movement. */
    virtual bool protectsParty() const { return false; }
    virtual Vec3 resolveMovement(const Vec3& /*from*/, const Vec3& to, f32 /*radius*/,
                                 f32 /*height*/) const {
        return to;
    }
    /** The attempted step matters when a resident's solid volume stops it. */
    virtual void touchScenery(s32 /*player*/, const Vec3& /*from*/, const Vec3& /*to*/,
                              f32 /*radius*/, f32 /*height*/) {}
    /** Fresh interaction presses for this tick, indexed by input seat. Proximity
     * and modal admission remain the native resident's responsibility. */
    virtual void interactionButtons(u32 /*pressedSeats*/) {}
    virtual std::optional<ImportedDialogue> takeDialogue() { return {}; }
    virtual std::vector<std::string> answerDialogue(s32 /*topic*/) { return {}; }
    virtual void dismissDialogue() {}
    virtual bool takeShopRequest() { return false; }
    /** Post-motion living visitors only. Grants and local transport are native
     * level rules; the common scene still owns inventory and departure effects. */
    virtual void updateParty(std::span<PlayerActor*> /*players*/, f32 /*seconds*/,
                             bool /*timeStopped*/) {}

    /** Retained floor owners for ordinary enemies, including attached corpses.
     * Native pressure bridges may remain solid until these occupants leave. */
    virtual void supportOpponents(std::span<const s32> /*floorOwners*/) {}
    virtual std::vector<ImportedPickup> takePickups() { return {}; }
    /** Ordinary melee, missiles and area effects share these native fixtures.
     * Inert bodies remain physical targets but never acquire an attack. */
    virtual std::vector<ImportedTarget> targets() const { return {}; }
    virtual bool strike(s32 /*target*/, f32 /*power*/, u32 /*flags*/, s32 /*byPlayer*/) {
        return false;
    }
    /** World collision can meet a breakable face before its attack proxy. */
    virtual bool strikeWorldObject(s32 /*object*/, f32 /*power*/, u32 /*flags*/, s32 /*byPlayer*/) {
        return false;
    }
    virtual std::vector<Obstacle> obstacles() const { return {}; }
    virtual std::span<const ImportedGenerator> importedGenerators() const { return {}; }
    virtual std::span<const ImportedActor> importedActors() const { return {}; }
    /** Admit contact-released native enemies into the ordinary pool. A refused
     * birth must remain pending rather than deleting its source fixture. */
    using EnemyRelease = std::function<bool(const EnemySpawn&)>;
    virtual void releaseEnemies(const EnemyRelease& /*release*/) {}
    /** Optional native body supplement, borrowed until this level unloads.
     * Movement/combat remain in the common pools, not in the asset adapter. */
    virtual ItemArchive* importedActorArchive(s32 /*kind*/) const { return nullptr; }
    virtual bool hasEnemyAssets() const { return false; }
    virtual std::unique_ptr<ItemArchive> loadEnemyAssets(s32 /*kind*/) const { return {}; }
    virtual const ImportedBoss* importedBoss() const { return nullptr; }
    /** Native support-trigger admission, before the boss's ordinary wake delay.
     * Existing importers and the base campaign remain immediately admitted. */
    virtual bool bossActivationReady(const Vec3& /*position*/) const { return true; }
    /** Campaign-local legendary weapons never consult or consume GDL relics. */
    virtual bool hasBossLegend(const CharacterSave& /*save*/) const { return false; }
    virtual bool consumeBossLegend(CharacterSave& /*save*/) { return false; }
    virtual ItemArchive* bossLegendArchive() const { return nullptr; }
    virtual const LegendProfile* bossLegendProfile() const { return nullptr; }
    /** True handles this phase, including deliberate silence, without donor audio. */
    virtual bool playBossLegendSound(LegendSound /*phase*/) { return false; }
    /** A native importer either supplies its complete body/moves or throws.
     * Null retains the original donor path for importers without native assets. */
    virtual std::unique_ptr<CombatantAssets>
    loadBossAssets(RenderDevice& /*device*/, const std::filesystem::path& /*commonRoot*/) const {
        return {};
    }
    /** First actual boss wake/intro, after ordinary arrival/admission holds. */
    virtual void bossIntro(const Vec3& /*position*/) {}
    /** Authored boss SFXX arena impulse, distinct from its initial wake. */
    virtual void bossArenaCue(const Vec3& /*position*/) {}
    /** The boss's death has played and its hold begins, the boss standing on `position`: told
     * once, before bossDefeated. */
    virtual void bossDeathPlayed(const Vec3& /*position*/) {}
    /** Native death completion, after animation/hold; never a lethal-hit grant. */
    virtual void bossDefeated(const Vec3& /*position*/, std::span<PlayerActor*> /*players*/) {}
    virtual std::vector<Mat4> arenaAnchors() const { return {}; }
    virtual std::vector<CombatArenaTarget> arenaTargets() const { return {}; }
    virtual void activateArena(const CombatArenaActivation& /*activation*/) {}
    virtual bool arenaBlocksSegment(const Vec3& /*from*/, const Vec3& /*to*/,
                                    f32 /*radius*/) const {
        return false;
    }
    virtual std::vector<MissileStop> arenaMissileStops() const { return {}; }
    virtual void updateGenerators(std::span<const ImportedGeneratorState> /*states*/) {}
    virtual void watch(const ViewVolume& /*view*/) {}
    virtual std::vector<ImportedHazardHit> takeHazardHits() { return {}; }
    virtual void affectArea(const ImportedAreaDamage& /*damage*/) {}
    /** True means the native level owns this boss cue, including an explicitly
     * unsupported cue. Callers must not silently replace it with another game's audio. */
    virtual bool playOpponentSound(std::string_view /*name*/, const Vec3& /*position*/) {
        return false;
    }
    virtual bool playEnemyFeedback(const EnemyFeedback& /*feedback*/, f32 /*health*/) {
        return false;
    }
    virtual bool playEnemyCue(const EnemyCue& /*cue*/) { return false; }
    virtual bool playEnemyMelee(s32 /*kind*/, s32 /*tier*/) { return false; }
    virtual std::optional<bool> suicideUsesGas() const { return {}; }
    virtual bool playDeathCue(const DeathEvent& /*cue*/) { return false; }
    virtual bool setDeathContact(bool /*active*/) { return false; }
    virtual bool playGeneratorFeedback(const GeneratorEvent& /*event*/) { return false; }
    /** Called outside the native tick; shared audio settings and pause apply. */
    /** Optional complete narration replacement. An empty result preserves the
     * entire original announcement, including its character-name prefix. */
    virtual std::optional<SoundSequence>
    gameplayAnnouncement(std::span<const std::string_view> /*lines*/,
                         std::optional<AnnouncementSubject> /*subject*/) const {
        return {};
    }
    virtual void playSounds(SoundPlayer& /*player*/, const Vec3& /*listener*/) {}
    struct MusicDuck {
        f32 seconds = 0;
        f32 scale = 1;
    };
    virtual std::optional<MusicDuck> ambientMusicDuck() const { return {}; }
    virtual void pauseSounds() {}
    virtual void stopSounds() {}
    virtual bool travelRequested() const { return false; }
    /** Failed destination load: retain gameplay and require fresh exit admission. */
    virtual bool cancelDeparture() { return false; }
    virtual std::optional<ImportedDeparture> departure(f32 /*alpha*/ = -1.0f) const {
        return std::nullopt;
    }
};

} // namespace gdl::game
