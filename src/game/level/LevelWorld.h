#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/AssetScope.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/ParticleField.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"
#include "engine/world/WorldScene.h"

#include "game/camera/TowerCamera.h"
#include "game/effects/ThermalTextures.h"
#include "game/level/DestructibleWalls.h"
#include "game/level/FallingScenery.h"
#include "game/level/HazardSurfaces.h"
#include "game/level/ImportedLevel.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelItemArchives.h"
#include "game/level/LevelTriggers.h"
#include "game/level/PlacedItems.h"
#include "game/level/Rotators.h"
#include "game/level/SkorneArena.h"
#include "game/level/TowerAccess.h"
#include "game/level/WorldDestruction.h"
#include "game/level/WorldMotion.h"

namespace gdl::game {

/**
 * One level of the game, loaded to be played in (Sumner's tower, the hub every adventure
 * starts from, unless told another): its geometry, collision and marker points, its moving
 * objects and flickering textures, the realm's item archive it borrows from, and from the
 * realm's data its light, its camera range and its sounds, shared by the screens that show
 * it.
 */
class LevelWorld {
    struct Simulation {
        s32 playerCount = -1;
        ParticleField particles;
        bool particlesAdvanced = false;
        std::vector<RotatorCue> rotatorCues;
        f32 frameRemainder = 0;
        f32 textureAdvance = 0;
        WorldLighting lighting;
        WorldLighting litNow;
        f32 ambientOffset = 0;
        bool operator==(const Simulation&) const = default;
        template <class Digest>
        friend void visitNativeState(Digest& digest, const Simulation& value) {
            digest.template fields<9>(value);
        }
    };

public:
    // One bound stage, with immutable layouts, meshes and textures retained in place.
    class LocalSnapshot {
        friend class LevelWorld;
        Simulation m_state;
        WorldScene::LocalSnapshot m_scene;
        WorldAnimator::LocalSnapshot m_worldAnimator;
        WorldCollision::LocalSnapshot m_collision;
        PlacedItems::LocalSnapshot m_placedItems;
        SkorneArena::LocalSnapshot m_skorneArena;
        FallingScenery::LocalSnapshot m_fallingScenery;
        DestructibleWalls::LocalSnapshot m_walls;
        WorldDestruction::LocalSnapshot m_destruction;
        LevelTriggers::LocalSnapshot m_triggers;
        Rotators::LocalSnapshot m_rotators;
        TextureAnimator::Clock m_textures;
        const LevelWorld* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    /** Exact captured native values of this bound world and all captured children. */
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    /** Refresh already identified stage/archive resources without creating assets. */
    bool registerNativeResources(NativeStateResources& resources) const;

    static constexpr std::string_view kPowerups = "POWERUPS";

    /** Loads the unpacked level; false (with a log line) when it is not there. Without the
     * realm's data the default light and camera range stand in. */
    bool load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const LevelRef& level = LevelRef::tower());
    /** Hosts an already decoded offline level without interpreting its native records as
     * GameCube items or campaign markers. Common character effects still use POWERUPS. */
    bool loadImported(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                      const LevelRef& level, WorldLayout layout, WorldCollision collision,
                      std::shared_ptr<ImportedLevel> imported);
    ImportedLevel* imported() { return m_imported.get(); }
    const ImportedLevel* imported() const { return m_imported.get(); }
    /** The level loaded. */
    const LevelRef& ref() const { return m_ref; }
    bool isTower() const { return m_ref.isTower(); }
    void clear();
    bool built() const { return m_imported != nullptr || m_scene.built(); }

    /** Moves the level's animated objects (and the collision that rides on them) and steps
     * its texture animations by `seconds`. */
    void update(f32 seconds, bool timeStopped = false);
    /** Begin a scene tick; particles retain their current picture if this tick is held. */
    void capturePresentation() {
        m_sim.particlesAdvanced = false;
        m_sim.textureAdvance = 0;
        m_scene.capturePresentation();
        m_placedItems.capturePresentation();
        if (m_imported) {
            m_imported->capturePresentation();
        }
    }
    /** Opens at once the gates a party already qualifies for, as the level starts; in the
     * tower, with `access`, the lifts down to the battlefield's portals too once its first
     * level is beaten (items.c 6956). */
    void startTriggers(std::span<const TriggerVisitor> visitors,
                       const TowerAccess* access = nullptr);
    /** Puts out the tower's glow at the portal to level `gate` of `world` (its
     * L1NSNC<letter><n>_ACTIVE object hidden, fn_8005B5B8's MBTreeSetFlags 2); false, with a
     * note, when the level has no such object, as the temple's and underworld's have none. */
    bool shutPortalGlow(s32 world, s32 gate);
    /** Fires the triggers the visitors stand in and carries the fields' fades on. */
    void updateTriggers(f32 seconds, std::span<const TriggerVisitor> visitors,
                        bool cameraHeld = false, std::span<const s32> occupiedFloors = {});
    const LevelTriggers& triggers() const { return m_triggers; }
    void activateTrigger(s32 id, bool atOnce);
    void shootTrigger(usize index) { m_triggers.shoot(index); }
    bool pressTrigger(usize index) { return m_triggers.press(index); }
    const Rotators& rotators() const { return m_rotators; }
    std::vector<RotatorCue> takeRotatorCues() { return std::exchange(m_sim.rotatorCues, {}); }
    std::vector<Vec3> takeWorldExplosions() { return m_destruction.takeExplosions(); }
    bool hasWorldExplosions() const { return m_destruction.hasExplosions(); }
    bool wouldExplodeObject(s32 object) const { return m_destruction.wouldExplode(object); }
    bool explodeObject(s32 object, const Vec3& position) {
        return m_destruction.explode(object, position, m_scene, m_collision);
    }
    const HazardSurfaces& hazards() const { return m_hazards; }
    std::vector<TriggerCameraCue> takeTriggerCameraCues() { return m_triggers.takeCameraCues(); }
    std::vector<Vec3> takeTriggerWakes() { return m_triggers.takeWakes(); }

    const WorldLayout& layout() const { return m_layout; }
    const WorldScene& scene() const { return m_scene; }
    const WorldAnimator& worldAnimator() const { return m_worldAnimator; }
    WorldMotion::State motion(bool pauseLoops = false) const;
    const TextureAnimator& textureAnimator() const { return m_textureAnimator; }
    const ParticleField& particles() const { return m_sim.particles; }
    bool particlesAdvanced() const { return m_sim.particlesAdvanced; }
    /** The pickups the level places: the crystals Sumner keeps for a new party. */
    const PlacedItems& placedItems() const { return m_placedItems; }
    PlacedItems& placedItems() { return m_placedItems; }
    std::vector<PlacedItems::PerkChange> blessItems(RenderDevice& device, const Vec3& position,
                                                    f32 radius, MagicPerk perk) {
        return m_placedItems.bless(device, position, radius, perk);
    }
    usize poisonFood(RenderDevice& device, const Vec3& position, f32 radius, f32 damage) {
        return m_placedItems.poisonFood(device, position, radius, damage);
    }
    std::vector<PlacedItems::BlastChange> blastItems(RenderDevice& device, const Vec3& position,
                                                     f32 radius, f32 damage,
                                                     bool destroysPickups = true) {
        return m_placedItems.blast(device, position, radius, damage, destroysPickups);
    }
    void attachItem(usize index, const Mat4& transform, bool contained) {
        m_placedItems.attach(index, transform, contained);
    }
    void discardItem(usize index) { m_placedItems.discard(index); }
    std::optional<s32> strikePotion(usize index, f32 damage) {
        return m_placedItems.strikePotion(index, damage);
    }
    /** Shows the pickups a party of `players` sees; none for the select screen's empty one. */
    void setPlayerCount(s32 players) {
        if (m_sim.playerCount == players) {
            return;
        }
        m_sim.playerCount = players;
        m_triggers.setPlayerCount(players);
        m_placedItems.setPlayerCount(players);
        m_walls.setPlayerCount(players, m_collision);
        m_fallingScenery.setPlayerCount(players);
    }
    const DestructibleWalls& walls() const { return m_walls; }
    void revealWall(usize index) { m_walls.reveal(index); }
    std::optional<s32> strikeWall(usize index, f32 power, u32 flags = 0) {
        return m_walls.strike(index, power, m_collision, flags);
    }
    /** Fades one of the level's objects (a unit); see WorldScene::setObjectAlpha. */
    void setObjectAlpha(usize object, f32 alpha) { m_scene.setObjectAlpha(object, alpha); }
    f32 objectAlpha(usize object) const { return m_scene.objectAlpha(object); }
    /** Hides/shows a separately controlled stage mesh without changing its collision.
     * False when absent or baked into static geometry. */
    bool setObjectVisible(std::string_view name, bool visible);
    void bossArenaCue(const Vec3& boss) { m_skorneArena.cue(boss); }
    const SkorneArena& skorneArena() const { return m_skorneArena; }
    /** The rocks and leaves that give way to a touch or a shot. */
    FallingScenery& fallingScenery() { return m_fallingScenery; }
    const FallingScenery& fallingScenery() const { return m_fallingScenery; }
    /** Hides the crystals until revealCrystals() brings them in. */
    void hideCrystals() { m_placedItems.hideCrystals(); }
    void revealCrystals(f32 seconds) { m_placedItems.reveal(seconds); }
    /** Takes what the collectors touch, starting the bursts. */
    std::vector<Pickup> collect(RenderDevice& device, std::span<const Collector> collectors,
                                const PickupJudge& judge = {}) {
        return m_placedItems.collect(device, collectors, judge);
    }
    /** Drops one of the level's items by its record's name at `position`. */
    bool placeItem(RenderDevice& device, std::string_view name, const Vec3& position) {
        return m_placedItems.place(device, name, position,
                                   m_collision.loaded() ? &m_collision : nullptr);
    }
    /** Drops the item of one of the level's records (what a chest held) at `position`. */
    bool placeItemRecord(RenderDevice& device, s32 record, const Vec3& position, s32 amount = 0) {
        return m_placedItems.placeRecord(device, record, position,
                                         m_collision.loaded() ? &m_collision : nullptr, amount);
    }
    void setItemOpener(usize index, s32 player) { m_placedItems.setOpener(index, player); }
    /** A pickup near `position` for one of the great ones to carry (PlacedItems::claim). */
    std::optional<usize> claimItem(const Vec3& position, f32 reach, f32 rise) {
        return m_placedItems.claim(position, reach, rise);
    }
    /** Lets a carried pickup go, thrown from `position` at `velocity`. */
    bool releaseItem(usize index, const Vec3& position, const Vec3& velocity, f32 noGrabSeconds) {
        return m_placedItems.release(index, position, velocity,
                                     m_collision.loaded() ? &m_collision : nullptr, noGrabSeconds);
    }
    /** Throws one of the level's items by its record's name from `position`, as a boss
     * spews its coins; it lands on the floor and cannot be taken for `noGrabSeconds`, and
     * holds `amount` when that is over none. */
    bool throwItem(RenderDevice& device, std::string_view name, const Vec3& position,
                   const Vec3& velocity, f32 noGrabSeconds,
                   std::optional<f32> strength = std::nullopt, s32 amount = 0) {
        return m_placedItems.throwItem(device, name, position, velocity,
                                       m_collision.loaded() ? &m_collision : nullptr, noGrabSeconds,
                                       strength, amount);
    }
    /** Whether any gold lies untaken. */
    bool goldLeft() const { return m_placedItems.goldLeft(); }
    /** The level's item archive, lending the torch flames and Sumner; empty when it is not
     * unpacked. */
    ItemArchive& items() { return m_itemArchives.primary(); }
    const ItemArchive& items() const { return m_itemArchives.primary(); }
    /** Stage textures borrowed by native item/enemy animation slots such as LAVA. */
    TextureSet& textures() { return m_textures; }
    ModelSet& models() { return m_models; }
    /** Common realm figures still available when a boss supplies its own item archive. */
    ItemArchive& realmItems() { return m_itemArchives.realm(); }
    /** The item archives in the order the original searches them: the level's, the realm's. */
    AssetScope itemScope() { return m_itemArchives.scope(); }
    ItemArchive& powerups() { return m_powerups; }
    bool hasItems() const { return m_itemArchives.loaded(); }
    const WorldCollision& collision() const { return m_collision; }
    /** The level's light, for everything standing in it. */
    /** The level's light as it is now, with whatever has been taken off its ambient. */
    const WorldLighting& lighting() const { return m_sim.litNow; }
    /** The level's own light, whatever has been taken off it: what effects are drawn by, so
     * that they stand out when the level goes dark. */
    const WorldLighting& fullLighting() const { return m_sim.lighting; }
    /** This frame's point lights, lighting the level and everything lit in it. */
    void setPointLights(std::span<const PointLight> points);
    /** Darkens everything lit by `offset` (-0.6 leaves two fifths of the light), the way the
     * original's ambient special darkens the picture. */
    void setAmbientOffset(f32 offset);
    f32 ambientOffset() const { return m_sim.ambientOffset; }
    /** Whether the level's record flags it dark: its ambient pinned out, the party's lanterns
     * its light (DoLighting, player.c 2499). */
    bool dark() const;
    /** What the follow camera takes from the level. */
    const CameraRange& cameraRange() const { return m_cameraRange; }
    /** The level's record, and its sound bank and music stream; null without the realm's
     * data. */
    const LevelInfo* level() const { return m_level; }
    const LevelAudioInfo* audio() const { return m_audio; }
    /** The authored world-impact sound, empty when the level specifies none. */
    std::string_view wallHitSound() const {
        return m_audio != nullptr ? m_worldData.soundName(m_audio->hitSound) : std::string_view{};
    }
    /** What the triggers refused or opened since last asked. */
    std::vector<TriggerRefusal> takeTriggerRefusals() { return m_triggers.takeRefusals(); }
    std::vector<TriggerLesson> takeTriggerLessons() { return m_triggers.takeLessons(); }
    std::vector<TriggerOpening> takeTriggerOpenings() { return m_triggers.takeOpenings(); }
    std::vector<TriggerOpening> takeTriggerSettled() { return m_triggers.takeSettled(); }
    bool hasLevelData() const { return m_level != nullptr; }

    /** The start camera paired with a spawn marker; missing indexed cameras fall back to
     * marker zero without changing the party's spawn. */
    std::optional<WorldCamera> entranceCamera(u32 startIndex = 0) const;

    /** The level's start marker number `index` (0 is its entrance). */
    const WorldLocator* startPoint(u32 index) const;
    /** Which of the tower's start markers a party back from realm `realm` stands at: the one
     * among that realm's portals, by the original's table; the entrance for any other. */
    static u32 towerMarkerOf(u32 realm);
    /** Where a party arriving from realm `realm` stands: in the tower among that realm's
     * portals (at the entrance instead when `access` says that realm's world is shut to it,
     * SetPlayerStartPos), anywhere else at the level's entrance. */
    const WorldLocator* arrivalPoint(u32 realm, const TowerAccess* access = nullptr) const;

    /** The game camera markers the follow camera takes its angles from. */
    const std::vector<WorldLocator>& cameraMarkers() const { return m_markers; }

    /** Draws the level as `camera` sees it: the geometry with its sorted objects farthest
     * first, then the particles facing the camera. */
    void draw(RenderDevice& device, const Mat4& clip, const WorldCamera& camera,
              f32 presentationAlpha = -1.0f) const {
        drawOpaque(device, clip, camera, presentationAlpha);
        drawDeferred(device, clip, camera, presentationAlpha);
    }
    void drawOpaque(RenderDevice& device, const Mat4& clip, const WorldCamera& camera,
                    f32 presentationAlpha = -1.0f) const {
        const CameraFrame frame = CameraFrame::of(camera);
        if (m_imported) {
            m_imported->draw(device, clip, camera, TreeModel::Pass::Opaque, presentationAlpha);
        }
        m_scene.drawOpaque(device, clip, frame, presentationAlpha,
                           presentedTextureFrameOffset(presentationAlpha));
        m_skorneArena.draw(device, clip, m_sim.litNow,
                           m_sim.particlesAdvanced ? presentationAlpha : -1.0f);
        m_fallingScenery.draw(device, clip, m_sim.litNow,
                              m_sim.particlesAdvanced ? presentationAlpha : -1.0f);
        m_walls.draw(device, clip, m_sim.litNow);
        m_triggers.draw(device, clip, m_sim.litNow);
        m_rotators.draw(device, clip, m_sim.litNow);
        m_placedItems.draw(device, clip, m_sim.litNow, &frame, TreeModel::Pass::Opaque,
                           presentationAlpha);
    }
    /** The pickups' alpha-textured bodies composite over the solids first; the veils
     * (the Trenches' haze) then blend over everything standing under them; the sorted
     * objects follow. The pickup parts the authored flags sort behind (a gem's halo and
     * crystal, which write no depth) come after the scenery's deferred queue, as the
     * original's lowered sort key orders them: the tower's rune strips, veils since they
     * wear alpha, and its sorted trees no longer paint over the glow of a gem standing
     * before them. The particles close. */
    void drawDeferred(RenderDevice& device, const Mat4& clip, const WorldCamera& camera,
                      f32 presentationAlpha = -1.0f) const {
        const CameraFrame frame = CameraFrame::of(camera);
        m_placedItems.draw(device, clip, m_sim.litNow, &frame, TreeModel::Pass::BlendedAhead,
                           presentationAlpha);
        if (m_imported) {
            m_imported->draw(device, clip, camera, TreeModel::Pass::Blended, presentationAlpha);
        }
        m_scene.drawVeils(device, clip, frame, presentationAlpha,
                          presentedTextureFrameOffset(presentationAlpha));
        m_scene.drawDeferred(device, clip, frame, presentationAlpha,
                             presentedTextureFrameOffset(presentationAlpha));
        m_placedItems.draw(device, clip, m_sim.litNow, &frame, TreeModel::Pass::BlendedBehind,
                           presentationAlpha);
        m_sim.particles.draw(device, clip, frame.right, frame.up,
                             m_sim.particlesAdvanced ? presentationAlpha : -1.0f, kThermalTextures);
    }

private:
    bool appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const;
    friend class TowerCameraCheckpoint;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
    u64 m_checkpointGeneration = 0;
    std::optional<f32> presentedTextureFrameOffset(f32 alpha) const;
    void loadLevelData(const std::filesystem::path& unpackedRoot);
    void applyAmbientOffset(f32 offset);
    LevelRef m_ref = LevelRef::tower();
    std::shared_ptr<ImportedLevel> m_imported;
    Simulation m_sim;
    void syncCollision();

    ModelSet m_models;
    TextureSet m_textures;
    AnimationSet m_animations; ///< the level's texture animations
    LevelItemArchives m_itemArchives;
    ItemArchive m_powerups;
    PlacedItems m_placedItems;
    SkorneArena m_skorneArena;
    FallingScenery m_fallingScenery;
    DestructibleWalls m_walls;
    WorldLayout m_layout;
    WorldScene m_scene;
    WorldAnimator m_worldAnimator;
    WorldDestruction m_destruction;
    TextureAnimator m_textureAnimator;
    LevelTriggers m_triggers;
    HazardSurfaces m_hazards;
    Rotators m_rotators;
    std::vector<s32> m_movingObjects; ///< objects whose collision follows their animation
    WorldCollision m_collision;
    WorldData m_worldData;
    CameraRange m_cameraRange;
    const LevelInfo* m_level = nullptr;
    const LevelAudioInfo* m_audio = nullptr;
    std::vector<WorldLocator> m_markers;
};

} // namespace gdl::game
