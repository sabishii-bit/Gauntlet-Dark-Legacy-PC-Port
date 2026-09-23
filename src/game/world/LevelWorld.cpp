#include "game/world/LevelWorld.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <vector>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {

bool LevelWorld::load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                      const LevelRef& level) {
    clear();
    m_ref = level;
    const std::filesystem::path directory = unpackedRoot / m_ref.directory;
    if (!std::filesystem::exists(directory / "world.json")) {
        log::info("Level: not unpacked ({}); run gdlunpack with --levels or --only <level>",
                  directory.string());
        return false;
    }
    loadLevelData(unpackedRoot);
    if (!m_layout.load(directory) || !m_models.load(directory) || !m_textures.load(directory)) {
        clear();
        return false;
    }
    if (!m_animations.load(directory)) {
        log::warn("Level: no animations manifest; its textures stand still");
    }
    // A boss level's own item archive (which holds the wizard who comes at the end) over
    // the realm's.
    const bool own = !m_ref.ownItems.empty() &&
                     std::filesystem::exists(unpackedRoot / m_ref.ownItems / "animations.json") &&
                     m_items.load(unpackedRoot / m_ref.ownItems);
    if (!own && !m_items.load(unpackedRoot / m_ref.items)) {
        log::warn(
            "Level: without the realm's item archive its borrowed textures and figures are absent");
    }
    if (own && m_ref.items != m_ref.ownItems &&
        std::filesystem::exists(unpackedRoot / m_ref.items / "animations.json")) {
        m_realmItems.load(unpackedRoot / m_ref.items);
    }
    // Boss-specific items take precedence, but realm textures (including torch particles)
    // remain available when that archive does not contain a requested bitmap.
    std::vector<TextureSet*> lenders;
    if (m_items.loaded()) {
        lenders.push_back(&m_items.textures);
    }
    if (m_realmItems.loaded()) {
        lenders.push_back(&m_realmItems.textures);
    }
    const std::span<TextureSet* const> lent{lenders};
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
        log::warn("Level: no collision; characters will walk through everything");
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
        log::warn("Level: without the powerups archive the pickups are absent");
    }
    const std::array<ItemArchive*, 2> archives{&m_items, &m_powerups};
    if (!m_placedItems.bind(device, m_layout, m_collision.loaded() ? &m_collision : nullptr,
                            archives)) {
        log::warn("Level: none of the level's pickups could be placed");
    }
    // The follow camera takes its angles from the game camera markers alone; the trigger
    // cameras are for the cuts.
    for (const WorldLocator& locator : m_layout.locators()) {
        if (locator.kind == LocatorKind::CameraGame) {
            m_markers.push_back(locator);
        }
    }
    log::info("Level: placed {} objects in {} batches and {} units ({} triangles), {} moving, "
              "{} texture animations, {} particle systems, {} pickups, {} triggers, {} collision "
              "triangles ({} moving objects), {} camera markers",
              m_scene.placedCount(), m_scene.batchCount(), m_scene.unitCount(),
              m_scene.triangleCount(), m_worldAnimator.size(), m_textureAnimator.size(),
              m_particles.size(), m_placedItems.size(), m_triggers.size(),
              m_collision.triangleCount(), m_collision.movingObjectCount(), m_markers.size());
    return true;
}

void LevelWorld::syncCollision() {
    for (const s32 object : m_movingObjects) {
        m_collision.setObjectTransform(object, m_scene.worldTransform(static_cast<usize>(object)));
    }
}

void LevelWorld::startTriggers(std::span<const TriggerVisitor> visitors) {
    m_triggers.openMet(visitors, m_worldAnimator, m_scene, &m_collision);
    m_worldAnimator.apply(m_scene);
    syncCollision();
}

void LevelWorld::updateTriggers(f32 seconds, std::span<const TriggerVisitor> visitors) {
    m_triggers.update(seconds, visitors, m_worldAnimator, m_scene, &m_collision);
}

void LevelWorld::update(f32 seconds) {
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
void LevelWorld::loadLevelData(const std::filesystem::path& unpackedRoot) {
    m_lighting = WorldLighting{};
    m_litNow = m_lighting;
    m_ambientOffset = 0.0f;
    m_cameraRange = CameraRange{};
    m_level = nullptr;
    m_audio = nullptr;
    const std::filesystem::path file = unpackedRoot / m_ref.worldDataFile();
    const LevelInfo* level = m_worldData.load(file) ? m_worldData.level(m_ref.name) : nullptr;
    if (level == nullptr) {
        log::warn("Level: no level {} in {}; the default light and camera range stand in",
                  m_ref.name, file.string());
        return;
    }
    m_level = level;
    m_lighting = WorldLighting::forLevel(*level);
    m_litNow = m_lighting;
    m_ambientOffset = 0.0f;
    if (const LevelCameraInfo* camera = m_worldData.camera(level->cameraIndex); camera != nullptr) {
        m_cameraRange.radiusMin = camera->radiusMin;
        m_cameraRange.radiusMax = camera->radiusMax;
        m_cameraRange.minPitch = camera->minPitch;
        m_cameraRange.boundsMin = camera->boundsMin;
        m_cameraRange.boundsMax = camera->boundsMax;
    }
    m_audio = m_worldData.audio(level->audioIndex);
}

void LevelWorld::clear() {
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
    m_realmItems.clear();
    m_frameRemainder = 0.0f;
    m_level = nullptr;
    m_audio = nullptr;
}

std::optional<WorldCamera> LevelWorld::entranceCamera() const {
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

void LevelWorld::setAmbientOffset(f32 offset) {
    if (offset == m_ambientOffset) {
        return;
    }
    m_ambientOffset = offset;
    // A level's light saturates what faces it, so taking the offset off the ambient term
    // alone would leave most of it as bright as ever; the original darkens the whole picture
    // with it, and so is everything lit here: the level's baked geometry through the scene,
    // what is lit as it is drawn through the light it is given. What glows is left alone.
    const f32 kept = std::clamp(1.0f + offset, 0.0f, 1.0f);
    m_litNow = m_lighting;
    m_litNow.ambient = m_lighting.ambient * kept;
    m_litNow.lightColor = m_lighting.lightColor * kept;
    m_scene.setDarken(1.0f - kept);
}

const WorldLocator* LevelWorld::startPoint(u32 index) const {
    return m_layout.findLocator(LocatorKind::Start, index);
}

u32 LevelWorld::towerMarkerOf(u32 realm) {
    constexpr std::array<u32, 14> kMarkers{0, 3, 2, 6, 5, 0, 0, 1, 0, 7, 8, 4, 0, 0};
    return realm < kMarkers.size() ? kMarkers[realm] : 0;
}

const WorldLocator* LevelWorld::arrivalPoint(u32 realm) const {
    const WorldLocator* marker = isTower() ? startPoint(towerMarkerOf(realm)) : nullptr;
    return marker != nullptr ? marker : startPoint(0);
}

} // namespace gdl::game
