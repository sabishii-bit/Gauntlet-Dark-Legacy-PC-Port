#include "game/world/TowerWorld.h"

#include <array>
#include <cmath>

#include "engine/core/Log.h"

namespace gdl::game {

bool TowerWorld::load(RenderDevice& device, const std::filesystem::path& unpackedRoot) {
    clear();
    const std::filesystem::path directory = unpackedRoot / kLevel;
    if (!std::filesystem::exists(directory / "world.json")) {
        log::info("Tower: the level is not unpacked ({}); run gdlunpack with --levels",
                  directory.string());
        return false;
    }
    loadLevelData(unpackedRoot);
    if (!m_layout.load(directory) || !m_models.load(directory) || !m_textures.load(directory)) {
        clear();
        return false;
    }
    if (!m_animations.load(directory)) {
        log::warn("Tower: no animations manifest; its textures stand still");
    }
    if (!m_items.load(unpackedRoot / kItems)) {
        log::warn("Tower: without the item archive the torches are unlit and Sumner absent");
    }
    // The item archive lends the level its external textures and the torches' frames.
    const std::array<TextureSet*, 1> lenders{&m_items.textures};
    const std::span<TextureSet* const> lent =
        m_items.loaded() ? std::span<TextureSet* const>{lenders} : std::span<TextureSet* const>{};
    if (!m_scene.build(m_layout, m_models, m_textures, device, m_lighting, lent)) {
        clear();
        return false;
    }
    m_worldAnimator.bind(m_layout);
    m_worldAnimator.apply(m_scene);
    m_textureAnimator.bind(m_animations.textureAnimations(), m_textures, device, lent);
    m_textureAnimator.apply(m_scene);
    m_particles.bind(m_layout, m_textures, device, lent);
    m_frameRemainder = 0.0f;
    if (!m_collision.load(directory, m_layout)) {
        log::warn("Tower: no collision; characters will walk through everything");
    }
    // Objects flagged to move keep their collision in their own space, placed by their
    // transform: the animated, and the force fields that only fade.
    for (usize i = 0; i < m_layout.objects().size(); ++i) {
        if ((m_layout.objects()[i].flags & WorldObject::kAnimated) != 0 || m_scene.moving(i)) {
            m_movingObjects.push_back(static_cast<s32>(i));
        }
    }
    m_collision.setMovingObjects(m_movingObjects);
    std::erase_if(m_movingObjects, [&](s32 object) { return !m_collision.moving(object); });
    m_triggers.bind(m_layout, m_worldAnimator, &m_collision);
    m_worldAnimator.apply(m_scene);
    syncCollision();
    if (!m_powerups.load(unpackedRoot / kPowerups)) {
        log::warn("Tower: without the powerups archive the crystals are absent");
    }
    const std::array<ItemArchive*, 2> archives{&m_items, &m_powerups};
    if (!m_placedItems.bind(device, m_layout, m_collision.loaded() ? &m_collision : nullptr,
                            archives)) {
        log::warn("Tower: none of the level's pickups could be placed");
    }
    // The follow camera takes its angles from the game camera markers alone; the trigger
    // cameras are for the cuts.
    for (const WorldLocator& locator : m_layout.locators()) {
        if (locator.kind == LocatorKind::CameraGame) {
            m_markers.push_back(locator);
        }
    }
    log::info("Tower: placed {} objects in {} batches and {} units ({} triangles), {} moving, "
              "{} texture animations, {} particle systems, {} pickups, {} triggers, {} collision "
              "triangles ({} moving objects), {} camera markers",
              m_scene.placedCount(), m_scene.batchCount(), m_scene.unitCount(),
              m_scene.triangleCount(), m_worldAnimator.size(), m_textureAnimator.size(),
              m_particles.size(), m_placedItems.size(), m_triggers.size(),
              m_collision.triangleCount(), m_collision.movingObjectCount(), m_markers.size());
    return true;
}

void TowerWorld::syncCollision() {
    for (const s32 object : m_movingObjects) {
        m_collision.setObjectTransform(object, m_scene.worldTransform(static_cast<usize>(object)));
    }
}

void TowerWorld::startTriggers(std::span<const TriggerVisitor> visitors) {
    m_triggers.openMet(visitors, m_worldAnimator, m_scene, &m_collision);
    m_worldAnimator.apply(m_scene);
    syncCollision();
}

void TowerWorld::updateTriggers(f32 seconds, std::span<const TriggerVisitor> visitors) {
    m_triggers.update(seconds, visitors, m_worldAnimator, m_scene, &m_collision);
}

void TowerWorld::update(f32 seconds) {
    if (!built()) {
        return;
    }
    m_worldAnimator.step(seconds, m_scene);
    syncCollision();
    m_particles.step(seconds);
    m_placedItems.update(seconds);
    // Texture animations count whole game frames.
    m_frameRemainder += seconds * WorldAnimator::kFramesPerSecond;
    const f32 frames = std::floor(m_frameRemainder);
    m_frameRemainder -= frames;
    m_textureAnimator.step(m_scene, static_cast<u32>(frames));
}

/** Takes the light, the camera range and the sounds from the realm's data. */
void TowerWorld::loadLevelData(const std::filesystem::path& unpackedRoot) {
    m_lighting = WorldLighting{};
    m_cameraRange = CameraRange{};
    m_level = nullptr;
    m_audio = nullptr;
    const std::filesystem::path file = unpackedRoot / kWorldData;
    const LevelInfo* level = m_worldData.load(file) ? m_worldData.level(kLevelName) : nullptr;
    if (level == nullptr) {
        log::warn("Tower: no level {} in {}; the default light and camera range stand in",
                  kLevelName, file.string());
        return;
    }
    m_level = level;
    m_lighting = WorldLighting::forLevel(*level);
    if (const LevelCameraInfo* camera = m_worldData.camera(level->cameraIndex);
        camera != nullptr) {
        m_cameraRange.radiusMin = camera->radiusMin;
        m_cameraRange.radiusMax = camera->radiusMax;
        m_cameraRange.minPitch = camera->minPitch;
        m_cameraRange.boundsMin = camera->boundsMin;
        m_cameraRange.boundsMax = camera->boundsMax;
    }
    m_audio = m_worldData.audio(level->audioIndex);
}

void TowerWorld::clear() {
    m_scene.clear();
    m_worldAnimator.clear();
    m_textureAnimator.clear();
    m_particles.clear();
    m_triggers.clear();
    m_movingObjects.clear();
    m_placedItems.clear();
    m_powerups.clear();
    m_collision.clear();
    m_markers.clear();
    m_textures.releaseTextures();
    m_items.clear();
    m_frameRemainder = 0.0f;
    m_level = nullptr;
    m_audio = nullptr;
}

std::optional<WorldCamera> TowerWorld::entranceCamera() const {
    const WorldLocator* locator = m_layout.findLocator(LocatorKind::CameraStart);
    if (locator == nullptr) {
        locator = m_layout.findLocator(LocatorKind::CameraGame);
    }
    if (locator == nullptr) {
        return std::nullopt;
    }
    WorldCamera camera;
    camera.position = locator->position;
    camera.pitch = locator->rotation.x;
    camera.yaw = locator->rotation.y;
    camera.roll = locator->rotation.z;
    return camera;
}

const WorldLocator* TowerWorld::startPoint(u32 world) const {
    return m_layout.findLocator(LocatorKind::Start, world);
}

} // namespace gdl::game
