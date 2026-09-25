#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/assets/AnimationSet.h"
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
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"
#include "engine/world/WorldScene.h"

#include "game/world/LevelCatalog.h"
#include "game/world/LevelTriggers.h"
#include "game/world/PlacedItems.h"
#include "game/world/SkorneArena.h"
#include "game/world/TowerCamera.h"

namespace gdl::game {

/**
 * One level of the game, loaded to be played in (Sumner's tower, the hub every adventure
 * starts from, unless told another): its geometry, collision and marker points, its moving
 * objects and flickering textures, the realm's item archive it borrows from, and from the
 * realm's data its light, its camera range and its sounds, shared by the screens that show
 * it.
 */
class LevelWorld {
public:
    static constexpr std::string_view kPowerups = "POWERUPS";

    /** Loads the unpacked level; false (with a log line) when it is not there. Without the
     * realm's data the default light and camera range stand in. */
    bool load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const LevelRef& level = LevelRef::tower());
    /** The level loaded. */
    const LevelRef& ref() const { return m_ref; }
    bool isTower() const { return m_ref.isTower(); }
    void clear();
    bool built() const { return m_scene.built(); }

    /** Moves the level's animated objects (and the collision that rides on them) and steps
     * its texture animations by `seconds`. */
    void update(f32 seconds);
    /** Opens at once the gates a party already qualifies for, as the level starts. */
    void startTriggers(std::span<const TriggerVisitor> visitors);
    /** Fires the triggers the visitors stand in and carries the fields' fades on. */
    void updateTriggers(f32 seconds, std::span<const TriggerVisitor> visitors);
    const LevelTriggers& triggers() const { return m_triggers; }

    const WorldLayout& layout() const { return m_layout; }
    const WorldScene& scene() const { return m_scene; }
    const WorldAnimator& worldAnimator() const { return m_worldAnimator; }
    const TextureAnimator& textureAnimator() const { return m_textureAnimator; }
    const ParticleField& particles() const { return m_particles; }
    /** The pickups the level places: the crystals Sumner keeps for a new party. */
    const PlacedItems& placedItems() const { return m_placedItems; }
    void attachItem(usize index, const Mat4& transform, bool contained) {
        m_placedItems.attach(index, transform, contained);
    }
    /** Shows the pickups a party of `players` sees; none for the select screen's empty one. */
    void setPlayerCount(s32 players) { m_placedItems.setPlayerCount(players); }
    /** Fades one of the level's objects (a unit); see WorldScene::setObjectAlpha. */
    void setObjectAlpha(usize object, f32 alpha) { m_scene.setObjectAlpha(object, alpha); }
    f32 objectAlpha(usize object) const { return m_scene.objectAlpha(object); }
    /** Hides/shows a separately controlled stage mesh without changing its collision.
     * False when absent or baked into static geometry. */
    bool setObjectVisible(std::string_view name, bool visible);
    void bossArenaCue(const Vec3& boss) { m_skorneArena.cue(boss); }
    const SkorneArena& skorneArena() const { return m_skorneArena; }
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
    /** Throws one of the level's items by its record's name from `position`, as a boss
     * spews its coins; it lands on the floor and cannot be taken for `noGrabSeconds`. */
    bool throwItem(RenderDevice& device, std::string_view name, const Vec3& position,
                   const Vec3& velocity, f32 noGrabSeconds,
                   std::optional<f32> strength = std::nullopt) {
        return m_placedItems.throwItem(device, name, position, velocity,
                                       m_collision.loaded() ? &m_collision : nullptr, noGrabSeconds,
                                       strength);
    }
    /** Whether any gold lies untaken. */
    bool goldLeft() const { return m_placedItems.goldLeft(); }
    /** The level's item archive, lending the torch flames and Sumner; empty when it is not
     * unpacked. */
    ItemArchive& items() { return m_items; }
    /** Common realm figures still available when a boss supplies its own item archive. */
    ItemArchive& realmItems() { return m_realmItems; }
    ItemArchive& powerups() { return m_powerups; }
    bool hasItems() const { return m_items.loaded(); }
    const WorldCollision& collision() const { return m_collision; }
    /** The level's light, for everything standing in it. */
    /** The level's light as it is now, with whatever has been taken off its ambient. */
    const WorldLighting& lighting() const { return m_litNow; }
    /** The level's own light, whatever has been taken off it: what effects are drawn by, so
     * that they stand out when the level goes dark. */
    const WorldLighting& fullLighting() const { return m_lighting; }
    /** Darkens everything lit by `offset` (-0.6 leaves two fifths of the light), the way the
     * original's ambient special darkens the picture. */
    void setAmbientOffset(f32 offset);
    f32 ambientOffset() const { return m_ambientOffset; }
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
    std::vector<TriggerOpening> takeTriggerOpenings() { return m_triggers.takeOpenings(); }
    std::vector<TriggerOpening> takeTriggerSettled() { return m_triggers.takeSettled(); }
    bool hasLevelData() const { return m_level != nullptr; }

    /** The camera the tower is first seen from: the first camera start marker. */
    std::optional<WorldCamera> entranceCamera() const;

    /** The level's start marker number `index` (0 is its entrance). */
    const WorldLocator* startPoint(u32 index) const;
    /** Which of the tower's start markers a party back from realm `realm` stands at: the one
     * among that realm's portals, by the original's table; the entrance for any other. */
    static u32 towerMarkerOf(u32 realm);
    /** Where a party arriving from realm `realm` stands: in the tower among that realm's
     * portals, anywhere else at the level's entrance. */
    const WorldLocator* arrivalPoint(u32 realm) const;

    /** The game camera markers the follow camera takes its angles from. */
    const std::vector<WorldLocator>& cameraMarkers() const { return m_markers; }

    /** Draws the level as `camera` sees it: the geometry with its sorted objects farthest
     * first, then the particles facing the camera. */
    void draw(RenderDevice& device, const Mat4& clip, const WorldCamera& camera) const {
        drawOpaque(device, clip, camera);
        drawDeferred(device, clip, camera);
    }
    void drawOpaque(RenderDevice& device, const Mat4& clip, const WorldCamera& camera) const {
        const CameraFrame frame = CameraFrame::of(camera);
        m_scene.drawOpaque(device, clip, frame);
        m_skorneArena.draw(device, clip, m_litNow);
        m_triggers.draw(device, clip, m_litNow);
        m_placedItems.draw(device, clip, m_litNow, &frame);
    }
    void drawDeferred(RenderDevice& device, const Mat4& clip, const WorldCamera& camera) const {
        const CameraFrame frame = CameraFrame::of(camera);
        m_scene.drawDeferred(device, clip, frame);
        m_particles.draw(device, clip, frame.right, frame.up);
    }

private:
    void loadLevelData(const std::filesystem::path& unpackedRoot);
    LevelRef m_ref = LevelRef::tower();
    void syncCollision();

    ModelSet m_models;
    TextureSet m_textures;
    AnimationSet m_animations; ///< the level's texture animations
    ItemArchive m_items;
    ItemArchive m_realmItems;
    ItemArchive m_powerups;
    PlacedItems m_placedItems;
    SkorneArena m_skorneArena;
    WorldLayout m_layout;
    WorldScene m_scene;
    WorldAnimator m_worldAnimator;
    TextureAnimator m_textureAnimator;
    ParticleField m_particles;
    LevelTriggers m_triggers;
    std::vector<s32> m_movingObjects; ///< objects whose collision follows their animation
    f32 m_frameRemainder = 0.0f;      ///< game frames owed to the texture animations
    WorldCollision m_collision;
    WorldData m_worldData;
    WorldLighting m_lighting;
    WorldLighting m_litNow; ///< m_lighting with the ambient offset
    f32 m_ambientOffset = 0.0f;
    CameraRange m_cameraRange;
    const LevelInfo* m_level = nullptr;
    const LevelAudioInfo* m_audio = nullptr;
    std::vector<WorldLocator> m_markers;
};

} // namespace gdl::game
