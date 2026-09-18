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

#include "game/world/LevelTriggers.h"
#include "game/world/PlacedItems.h"
#include "game/world/TowerCamera.h"

namespace gdl::game {

/**
 * Sumner's tower, the hub every adventure starts from: the level's geometry, collision and
 * marker points, its moving objects and flickering textures, the item archive it borrows
 * from, and from the realm's data its light, its camera range and its sounds, loaded once
 * and shared by the screens that show it.
 */
class TowerWorld {
public:
    static constexpr std::string_view kLevel = "LEVELS/LEVELL1";
    static constexpr std::string_view kItems = "ITEMS/LEVELL";
    static constexpr std::string_view kPowerups = "POWERUPS";
    static constexpr std::string_view kWorldData = "wdata/TOWER.json";
    static constexpr std::string_view kLevelName = "L1";

    /** Loads the unpacked level; false (with a log line) when it is not there. Without the
     * realm's data the default light and camera range stand in. */
    bool load(RenderDevice& device, const std::filesystem::path& unpackedRoot);
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
    /** Shows the pickups a party of `players` sees; none for the select screen's empty one. */
    void setPlayerCount(s32 players) { m_placedItems.setPlayerCount(players); }
    /** Fades one of the level's objects (a unit); see WorldScene::setObjectAlpha. */
    void setObjectAlpha(usize object, f32 alpha) { m_scene.setObjectAlpha(object, alpha); }
    f32 objectAlpha(usize object) const { return m_scene.objectAlpha(object); }
    /** Hides the crystals until revealCrystals() brings them in. */
    void hideCrystals() { m_placedItems.hideCrystals(); }
    void revealCrystals(f32 seconds) { m_placedItems.reveal(seconds); }
    /** Takes what the collectors touch, starting the bursts. */
    std::vector<Pickup> collect(RenderDevice& device, std::span<const Collector> collectors) {
        return m_placedItems.collect(device, collectors);
    }
    /** The level's item archive, lending the torch flames and Sumner; empty when it is not
     * unpacked. */
    ItemArchive& items() { return m_items; }
    bool hasItems() const { return m_items.loaded(); }
    const WorldCollision& collision() const { return m_collision; }
    /** The level's light, for everything standing in it. */
    const WorldLighting& lighting() const { return m_lighting; }
    /** What the follow camera takes from the level. */
    const CameraRange& cameraRange() const { return m_cameraRange; }
    /** The level's record, and its sound bank and music stream; null without the realm's
     * data. */
    const LevelInfo* level() const { return m_level; }
    const LevelAudioInfo* audio() const { return m_audio; }
    /** What the triggers refused or opened since last asked. */
    std::vector<TriggerRefusal> takeTriggerRefusals() { return m_triggers.takeRefusals(); }
    std::vector<TriggerOpening> takeTriggerOpenings() { return m_triggers.takeOpenings(); }
    std::vector<TriggerOpening> takeTriggerSettled() { return m_triggers.takeSettled(); }
    bool hasLevelData() const { return m_level != nullptr; }

    /** The camera the tower is first seen from: the first camera start marker. */
    std::optional<WorldCamera> entranceCamera() const;

    /** Where players arriving from world `world` stand (0 is the tower's own entrance). */
    const WorldLocator* startPoint(u32 world) const;

    /** The game camera markers the follow camera takes its angles from. */
    const std::vector<WorldLocator>& cameraMarkers() const { return m_markers; }

    /** Draws the level as `camera` sees it: the geometry with its sorted objects farthest
     * first, then the particles facing the camera. */
    void draw(RenderDevice& device, const Mat4& clip, const WorldCamera& camera) const {
        const CameraFrame frame = CameraFrame::of(camera);
        m_scene.draw(device, clip, frame);
        m_placedItems.draw(device, clip, m_lighting, &frame);
        m_particles.draw(device, clip, frame.right, frame.up);
    }

private:
    void loadLevelData(const std::filesystem::path& unpackedRoot);
    void syncCollision();

    ModelSet m_models;
    TextureSet m_textures;
    AnimationSet m_animations; ///< the level's texture animations
    ItemArchive m_items;
    ItemArchive m_powerups;
    PlacedItems m_placedItems;
    WorldLayout m_layout;
    WorldScene m_scene;
    WorldAnimator m_worldAnimator;
    TextureAnimator m_textureAnimator;
    ParticleField m_particles;
    LevelTriggers m_triggers;
    std::vector<s32> m_movingObjects; ///< objects whose collision follows their animation
    f32 m_frameRemainder = 0.0f; ///< game frames owed to the texture animations
    WorldCollision m_collision;
    WorldData m_worldData;
    WorldLighting m_lighting;
    CameraRange m_cameraRange;
    const LevelInfo* m_level = nullptr;
    const LevelAudioInfo* m_audio = nullptr;
    std::vector<WorldLocator> m_markers;
};

} // namespace gdl::game
