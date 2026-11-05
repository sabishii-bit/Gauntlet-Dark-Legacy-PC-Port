#include "game/level/LevelWorld.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/platform/Paths.h"

#include "game/effects/DynamicLights.h"
#include "game/level/BossKinds.h"
#include "game/level/NativeStateResources.h"

namespace gdl::game {

namespace {

constexpr Vec3 kCameraBoundsMinInset{8, 0, 8};
constexpr Vec3 kCameraBoundsMaxInset{-8, 4, -8};

void makeMoltenBallsContactOnly(const WorldLayout& layout, WorldCollision& collision) {
    const auto& objects = layout.objects();
    for (usize i = 0; i < objects.size(); ++i) {
        if (objects[i].name.starts_with("F1FFNSXCLAVA#")) {
            collision.setContactOnly(static_cast<s32>(i), true);
        }
        if (!objects[i].name.starts_with("F1NSLAVABALL#")) {
            continue;
        }
        const s32 root = objects[i].parent;
        if (root < 0) {
            continue;
        }
        for (usize child = 0; child < objects.size(); ++child) {
            s32 at = static_cast<s32>(child);
            for (usize guard = 0;
                 at >= 0 && static_cast<usize>(at) < objects.size() && guard < objects.size();
                 ++guard) {
                if (at == root) {
                    collision.setContactOnly(static_cast<s32>(child), true);
                    break;
                }
                at = objects[static_cast<usize>(at)].parent;
            }
        }
    }
}

} // namespace

bool LevelWorld::appendDigest(NativeStateDigest& digest, const LocalSnapshot* saved) const {
    if (m_imported) {
        // Native presentation/trigger state has no rollback contract yet. Never
        // authenticate a partial imported world as an ordinary network stage.
        digest.invalidate();
        return false;
    }
    digest.string("LevelWorld/1");
    digest.value(m_ref);
    digest.resource(&m_layout);
    digest.resource(&m_collision);
    digest.values(m_movingObjects, saved ? saved->m_state : m_sim);
    digest.fields<5>(m_cameraRange);
    digest.wide(m_markers.size());
    for (const auto& marker : m_markers) {
        digest.fields<5>(marker);
    }
    m_textureAnimator.appendBindingDigest(digest);
    digest.fields<3>(saved ? saved->m_textures : m_textureAnimator.clock());
    if (saved) {
        m_scene.appendLocalDigest(digest, saved->m_scene);
        m_worldAnimator.appendLocalDigest(digest, saved->m_worldAnimator);
        m_collision.appendLocalDigest(digest, saved->m_collision);
        m_placedItems.appendLocalDigest(digest, saved->m_placedItems);
        m_skorneArena.appendLocalDigest(digest, saved->m_skorneArena);
        m_fallingScenery.appendLocalDigest(digest, saved->m_fallingScenery);
        m_walls.appendLocalDigest(digest, saved->m_walls);
        m_destruction.appendLocalDigest(digest, saved->m_destruction);
        m_triggers.appendLocalDigest(digest, saved->m_triggers);
        m_rotators.appendLocalDigest(digest, saved->m_rotators);
    } else {
        m_scene.appendNativeDigest(digest);
        m_worldAnimator.appendNativeDigest(digest);
        m_collision.appendNativeDigest(digest);
        m_placedItems.appendNativeDigest(digest);
        m_skorneArena.appendNativeDigest(digest);
        m_fallingScenery.appendNativeDigest(digest);
        m_walls.appendNativeDigest(digest);
        m_destruction.appendNativeDigest(digest);
        m_triggers.appendNativeDigest(digest);
        m_rotators.appendNativeDigest(digest);
    }
    // Layout/art/world-data are immutable for this epoch. Their contract and
    // semantic resource identities are authenticated before admitting history.
    return digest.good();
}

bool LevelWorld::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, nullptr);
}

bool LevelWorld::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    // Guard the native snapshot roster too; owner/epoch are admission, not peer state.
    [[maybe_unused]] const auto& [state, scene, animator, collision, items, arena, falling, walls,
                                  destruction, triggers, rotators, textures, owner, binding] =
        saved;
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, &saved);
}

bool LevelWorld::registerNativeResources(NativeStateResources& resources) const {
    return resources.refreshTextures(m_textures) &&
           (!m_powerups.loaded() || resources.refreshArchive(m_powerups)) &&
           m_placedItems.registerNativeResources(resources) &&
           m_triggers.registerNativeResources(resources) &&
           m_rotators.registerNativeResources(resources);
}

void LevelWorld::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner = this;
    saved.m_binding = m_checkpointBinding;
    saved.m_state = m_sim;
    saved.m_textures = m_textureAnimator.clock();
    m_scene.saveLocal(saved.m_scene);
    m_worldAnimator.saveLocal(saved.m_worldAnimator);
    m_collision.saveLocal(saved.m_collision);
    m_placedItems.saveLocal(saved.m_placedItems);
    m_skorneArena.saveLocal(saved.m_skorneArena);
    m_fallingScenery.saveLocal(saved.m_fallingScenery);
    m_walls.saveLocal(saved.m_walls);
    m_destruction.saveLocal(saved.m_destruction);
    m_triggers.saveLocal(saved.m_triggers);
    m_rotators.saveLocal(saved.m_rotators);
}

bool LevelWorld::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto binding = saved.m_binding.lock();
    return !m_imported && saved.m_owner == this && binding && binding == m_checkpointBinding &&
           m_scene.canRestoreLocal(saved.m_scene) &&
           m_worldAnimator.canRestoreLocal(saved.m_worldAnimator) &&
           m_collision.canRestoreLocal(saved.m_collision) &&
           m_placedItems.canRestoreLocal(saved.m_placedItems) &&
           m_skorneArena.canRestoreLocal(saved.m_skorneArena) &&
           m_fallingScenery.canRestoreLocal(saved.m_fallingScenery) &&
           m_walls.canRestoreLocal(saved.m_walls) &&
           m_destruction.canRestoreLocal(saved.m_destruction) &&
           m_triggers.canRestoreLocal(saved.m_triggers) &&
           m_rotators.canRestoreLocal(saved.m_rotators);
}

bool LevelWorld::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_state == right.m_state &&
           left.m_textures == right.m_textures && m_scene.equalLocal(left.m_scene, right.m_scene) &&
           m_worldAnimator.equalLocal(left.m_worldAnimator, right.m_worldAnimator) &&
           m_collision.equalLocal(left.m_collision, right.m_collision) &&
           m_placedItems.equalLocal(left.m_placedItems, right.m_placedItems) &&
           m_skorneArena.equalLocal(left.m_skorneArena, right.m_skorneArena) &&
           m_fallingScenery.equalLocal(left.m_fallingScenery, right.m_fallingScenery) &&
           m_walls.equalLocal(left.m_walls, right.m_walls) &&
           m_destruction.equalLocal(left.m_destruction, right.m_destruction) &&
           m_triggers.equalLocal(left.m_triggers, right.m_triggers) &&
           m_rotators.equalLocal(left.m_rotators, right.m_rotators);
}

bool LevelWorld::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    // Validate the bound stage before touching any child. Restores only copy native
    // values or rebuild derived collision/draw caches; they never fire gameplay events.
    m_sim = saved.m_state;
    m_textureAnimator.restoreClock(saved.m_textures);
    m_scene.restoreLocal(saved.m_scene);
    m_worldAnimator.restoreLocal(saved.m_worldAnimator);
    m_collision.restoreLocal(saved.m_collision);
    m_placedItems.restoreLocal(saved.m_placedItems);
    m_skorneArena.restoreLocal(saved.m_skorneArena);
    m_fallingScenery.restoreLocal(saved.m_fallingScenery);
    m_walls.restoreLocal(saved.m_walls);
    m_destruction.restoreLocal(saved.m_destruction);
    m_triggers.restoreLocal(saved.m_triggers);
    m_rotators.restoreLocal(saved.m_rotators);
    return true;
}

bool LevelWorld::load(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                      const LevelRef& level) {
    clear();
    m_ref = level;
    const std::filesystem::path directory = unpackedRoot / paths::fromUtf8(m_ref.directory);
    const AssetLocator levelFiles(directory);
    if (!levelFiles.find("worlds.ps2") && !levelFiles.find("world.json")) {
        log::info("Level: no world data under {}", paths::utf8(directory));
        return false;
    }
    if (!m_layout.load(directory) || !m_models.load(directory) || !m_textures.load(directory)) {
        clear();
        return false;
    }
    loadLevelData(unpackedRoot);
    if (!m_animations.load(directory)) {
        log::warn("Level: no animation data; its textures stand still");
    }
    if (!m_itemArchives.load(unpackedRoot, m_ref)) {
        log::warn(
            "Level: without the realm's item archive its borrowed textures and figures are absent");
    }
    const auto lenders = m_itemArchives.textureLenders();
    const std::span<TextureSet* const> lent{lenders};
    std::vector<usize> controlledObjects;
    m_destruction.bind(m_layout);
    controlledObjects.assign(m_destruction.controlledObjects().begin(),
                             m_destruction.controlledObjects().end());
    // Fading trigger targets are not always flagged as animated. Keep their
    // complete subtree individually drawable rather than baking it into a batch.
    for (const auto& instance : m_layout.itemInstances()) {
        if (instance.info < 0 || static_cast<usize>(instance.info) >= m_layout.itemInfos().size() ||
            m_layout.itemInfos()[static_cast<usize>(instance.info)].type != ItemInfo::kTrigger) {
            continue;
        }
        const usize target = instance.params[0] | (static_cast<usize>(instance.params[1]) << 8U);
        if (target >= m_layout.objects().size()) {
            continue;
        }
        for (usize i = 0; i < m_layout.objects().size(); ++i) {
            auto at = static_cast<s32>(i);
            for (usize guard = 0; at >= 0 && guard < m_layout.objects().size(); ++guard) {
                if (static_cast<usize>(at) == target) {
                    controlledObjects.push_back(i);
                    break;
                }
                at = m_layout.objects()[static_cast<usize>(at)].parent;
            }
        }
    }
    std::ranges::sort(controlledObjects);
    const auto duplicates = std::ranges::unique(controlledObjects);
    controlledObjects.erase(duplicates.begin(), duplicates.end());
    const std::string_view arenaObject =
        bossArenaObject(m_level != nullptr ? m_level->bossType : -1);
    if (!arenaObject.empty()) {
        for (usize i = 0; i < m_layout.objects().size(); ++i) {
            if (m_layout.objects()[i].name == arenaObject) {
                controlledObjects.push_back(i);
                break;
            }
        }
    }
    // The tower's portal glows are put out one by one for the portals it keeps shut.
    if (m_ref.isTower()) {
        for (usize i = 0; i < m_layout.objects().size(); ++i) {
            if (TowerAccess::isGlowObject(m_layout.objects()[i].name)) {
                controlledObjects.push_back(i);
            }
        }
    }
    // A5's lightning sheets cross the arena in mesh space but belong behind it.
    // Keep this presentation override local: the same sort-behind flag is used
    // by L1's forcefields, reflections and glows, which must blend AFTER solids.
    // In AddSortObject it biases the deferred queue; it never selects a backdrop.
    std::vector<usize> backgroundObjects;
    if (m_ref.name == "A5") {
        constexpr u32 kLightningLayer = WorldObject::kSorted | WorldObject::kNoDepthWrite |
                                        WorldObject::kSortBehind | WorldObject::kSortBack;
        for (usize i = 0; i < m_layout.objects().size(); ++i) {
            if ((m_layout.objects()[i].objectFlags & kLightningLayer) == kLightningLayer) {
                backgroundObjects.push_back(i);
            }
        }
    }
    std::vector<usize> cameraFadeObjects;
    if (m_ref.name == "E1") {
        // Intentional visibility improvement: fade the broken column tops along
        // with the nearby shafts. Native caps have no fade flag; keep the disc
        // data, collision, intact tops, pedestals and shaft materials unchanged.
        constexpr std::array<std::string_view, 7> kBrokenCaps{
            "E1COLUMN#02", "E1COLUMN#05", "E1COLUMN#20", "E1COLUMN#22",
            "E1COLUMN#25", "E1COLUMN#28", "E1COLUMN#29"};
        for (usize i = 0; i < m_layout.objects().size(); ++i) {
            if (std::ranges::find(kBrokenCaps, m_layout.objects()[i].name) != kBrokenCaps.end()) {
                cameraFadeObjects.push_back(i);
            }
        }
    }
    // The collision comes first: the scene asks it which translucent sheets lie on a floor.
    if (!m_collision.load(directory, m_layout)) {
        log::warn("Level: no collision; characters will walk through everything");
    }
    if (!m_scene.build(m_layout, m_models, m_textures, device, m_sim.lighting, lent,
                       controlledObjects, backgroundObjects, cameraFadeObjects,
                       m_collision.loaded() ? &m_collision : nullptr, kThermalTextures)) {
        clear();
        return false;
    }
    applyAmbientOffset(dark() ? -1.0f : 0.0f); // the Trenches start dark, not on a first strike
    m_worldAnimator.bind(m_layout);
    if (const BossStage* stage = bossStageOf(m_level != nullptr ? m_level->bossType : -1);
        stage != nullptr && stage->fallingMasonry) {
        m_skorneArena.bind(device, m_layout, m_models, m_textures);
    }
    m_fallingScenery.bind(device, m_layout, m_models, m_textures, m_ref.name);
    m_worldAnimator.apply(m_scene);
    m_textureAnimator.bind(m_animations.textureAnimations(), m_textures, device, lent);
    m_textureAnimator.apply(m_scene);
    m_sim.particles.bind(m_layout, m_textures, device, lent);
    m_sim.frameRemainder = 0.0f;
    // Objects flagged to move keep their collision in their own space, placed by their
    // transform: the animated, and the force fields that only fade.
    for (usize i = 0; i < m_layout.objects().size(); ++i) {
        if ((m_layout.objects()[i].flags & WorldObject::kAnimated) != 0 || m_scene.moving(i)) {
            m_movingObjects.push_back(static_cast<s32>(i));
        }
    }
    m_collision.setMovingObjects(m_movingObjects);
    syncCollision();
    m_triggers.bind(m_layout, m_worldAnimator, &m_collision);
    std::erase_if(m_movingObjects, [&](s32 object) { return !m_collision.moving(object); });
    m_hazards.bind(m_layout);
    if (m_ref.name == "F1") {
        // Requested damage-only molten balls: retail marks these animated bodies and
        // their glow planes as walls. Keep their contact damage, not solid-body response.
        makeMoltenBallsContactOnly(m_layout, m_collision);
    }
    // A level with an archive of its own (A1's holds only the tentacle) still takes its
    // switch art from the realm's, the second header the original searches.
    const AssetScope scope = itemScope();
    m_triggers.bindFigures(device, m_layout, scope);
    m_rotators.bind(m_layout);
    m_rotators.bindFigures(device, m_layout, scope);
    m_worldAnimator.apply(m_scene);
    m_triggers.placeTargets(m_scene, &m_collision);
    syncCollision();
    m_sim.particles.syncNodes(m_scene);
    if (!m_powerups.load(unpackedRoot / kPowerups)) {
        log::warn("Level: without the powerups archive the pickups are absent");
    }
    const std::array<ItemArchive*, 2> archives{&items(), &m_powerups};
    if (!m_placedItems.bind(device, m_layout, m_collision.loaded() ? &m_collision : nullptr,
                            archives)) {
        log::warn("Level: none of the level's pickups could be placed");
    }
    // Initial pickup placement probes world floors, not breakable-item roofs. Append
    // those solid walls afterward so enclosed food stays inside its box, while live
    // collision still blocks actors and attacks. Existing moving-floor bindings survive.
    m_walls.bind(device, m_layout, m_models, m_textures, m_collision);
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
              m_sim.particles.size(), m_placedItems.size(), m_triggers.size(),
              m_collision.triangleCount(), m_collision.movingObjectCount(), m_markers.size());
    return true;
}

bool LevelWorld::loadImported(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                              const LevelRef& level, WorldLayout layout, WorldCollision collision,
                              std::shared_ptr<ImportedLevel> imported) {
    // The native adapter owns its own fixture and campaign semantics. Keeping
    // their records out of the GC helper rosters prevents e.g. a native rune
    // index from becoming a GDL runestone or a native switch becoming a GC gate.
    if (level.realmId != -1 || !imported || !collision.loaded() || !layout.itemInfos().empty() ||
        !layout.itemInstances().empty() || !layout.animations().empty() ||
        !layout.particleTemplates().empty()) {
        log::warn("Imported level: missing collision/presentation or incompatible common metadata");
        return false;
    }
    clear();
    m_ref = level;
    m_layout = std::move(layout);
    m_collision = std::move(collision);
    // Imported owners may borrow this address; bind only the final member,
    // never the by-value parameter that is destroyed when loading returns.
    imported->bindCollision(m_collision);
    m_models = ModelSet{};
    m_textures = TextureSet{};
    m_animations = AnimationSet{};
    m_worldData = WorldData{};
    m_sim = Simulation{};
    m_cameraRange = CameraRange{};
    m_cameraRange.boundsMin = m_layout.minBounds();
    m_cameraRange.boundsMax = m_layout.maxBounds();
    m_worldAnimator.bind(m_layout);
    m_triggers.bind(m_layout, m_worldAnimator, &m_collision);
    m_hazards.bind(m_layout);
    m_rotators.bind(m_layout);
    if (!m_powerups.load(unpackedRoot / kPowerups)) {
        log::warn("Imported level: common POWERUPS archive is absent");
    }
    const std::array<ItemArchive*, 1> archives{&m_powerups};
    m_placedItems.bind(device, m_layout, &m_collision, archives);
    for (const auto& locator : m_layout.locators()) {
        if (locator.kind == LocatorKind::CameraGame) {
            m_markers.push_back(locator);
        }
    }
    // Publish only after all borrowed common bindings have been established.
    m_imported = std::move(imported);
    return true;
}

void LevelWorld::syncCollision() {
    for (const s32 object : m_movingObjects) {
        m_collision.setObjectTransform(object, m_scene.worldTransform(static_cast<usize>(object)));
    }
    if (m_imported) {
        m_imported->syncCollision(m_collision);
    }
    m_placedItems.syncFloors();
}

WorldMotion::State LevelWorld::motion(bool pauseLoops) const {
    return WorldMotion::capture(m_worldAnimator, m_triggers, m_rotators, m_collision,
                                m_layout.objects().size(), pauseLoops);
}

bool LevelWorld::setObjectVisible(std::string_view name, bool visible) {
    for (usize i = 0; i < m_layout.objects().size(); ++i) {
        if (m_layout.objects()[i].name == name) {
            return m_scene.setObjectVisible(i, visible);
        }
    }
    return false;
}

bool LevelWorld::shutPortalGlow(s32 world, s32 gate) {
    const std::string name = TowerAccess::glowObjectName(world, gate);
    if (setObjectVisible(name, false)) {
        return true;
    }
    log::info("Level: no {} to put out for the shut portal", name);
    return false;
}

void LevelWorld::startTriggers(std::span<const TriggerVisitor> visitors,
                               const TowerAccess* access) {
    // The province uses crystal gate index 1, not its realm id (7). Retail omits
    // these tower-only pickups if any active character has earned that gate.
    if (isTower() && std::ranges::any_of(visitors, [](const TriggerVisitor& visitor) {
            return LevelTriggers::crystalsMet(visitor, 1);
        })) {
        m_placedItems.retireCrystals();
    }
    m_triggers.openMet(visitors, m_worldAnimator, m_scene, &m_collision);
    if (isTower() && access != nullptr && access->liftsOpen()) {
        m_triggers.openAtOnce(TowerAccess::kLiftTriggers, m_worldAnimator, m_scene, &m_collision);
    }
    if (isTower() && std::ranges::any_of(visitors, [](const TriggerVisitor& visitor) {
            return visitor.sumner || visitor.gargoylePieces[2] < 0 ||
                   visitor.gargoylePieces[2] >= Relics::kGargoyleNeeded[2];
        })) {
        for (const s32 id : TowerAccess::kLiftTriggers) {
            m_triggers.activate(id, WorldAnimator::Finish::AtOnce, m_worldAnimator, m_scene,
                                &m_collision);
        }
    }
    m_worldAnimator.apply(m_scene);
    syncCollision();
    m_sim.particles.syncNodes(m_scene);
    m_placedItems.snapPresentation();
}

void LevelWorld::activateTrigger(s32 id, WorldAnimator::Finish finish) {
    m_triggers.activate(id, finish, m_worldAnimator, m_scene, &m_collision);
    m_worldAnimator.apply(m_scene);
    syncCollision();
    m_sim.particles.syncNodes(m_scene);
    if (finish == WorldAnimator::Finish::AtOnce) {
        m_placedItems.snapPresentation();
    }
}

void LevelWorld::updateTriggers(f32 seconds, std::span<const TriggerVisitor> visitors,
                                bool cameraHeld, std::span<const s32> occupiedFloors) {
    m_triggers.setCameraHeld(cameraHeld);
    m_triggers.update(seconds, visitors, m_worldAnimator, m_scene, &m_collision, occupiedFloors);
    std::vector<RotatorCue> cues = m_rotators.update(seconds, visitors, m_scene);
    m_sim.rotatorCues.insert(m_sim.rotatorCues.end(), cues.begin(), cues.end());
    syncCollision();
    m_sim.particles.syncNodes(m_scene);
}

void LevelWorld::update(f32 seconds, bool timeStopped) {
    if (!built()) {
        return;
    }
    if (m_imported) {
        m_imported->update(seconds, timeStopped);
    }
    m_worldAnimator.step(seconds, m_scene, timeStopped);
    m_destruction.update(m_worldAnimator.cycleEvents(), m_scene, m_collision);
    m_skorneArena.update(seconds);
    m_fallingScenery.update(seconds);
    m_walls.update(seconds);
    syncCollision();
    m_sim.particles.syncNodes(m_scene);
    m_sim.particles.step(seconds);
    m_sim.particlesAdvanced = seconds > 0;
    m_placedItems.update(seconds);
    // Texture animations count whole game frames.
    m_sim.textureAdvance = std::max(seconds, 0.0f) * WorldAnimator::kFramesPerSecond;
    m_sim.frameRemainder += m_sim.textureAdvance;
    const f32 frames = std::floor(m_sim.frameRemainder);
    m_sim.frameRemainder -= frames;
    m_textureAnimator.step(m_scene, static_cast<u32>(frames));
}

std::optional<f32> LevelWorld::presentedTextureFrameOffset(f32 alpha) const {
    if (alpha < 0) {
        return std::nullopt;
    }
    return m_sim.frameRemainder - m_sim.textureAdvance * (1.0f - std::clamp(alpha, 0.0f, 1.0f));
}

/** Takes the light, the camera range and the sounds from the realm's data. */
void LevelWorld::loadLevelData(const std::filesystem::path& unpackedRoot) {
    m_sim.lighting = WorldLighting{};
    m_sim.litNow = m_sim.lighting;
    m_sim.ambientOffset = 0.0f;
    m_cameraRange = CameraRange{};
    m_level = nullptr;
    m_audio = nullptr;
    const std::filesystem::path file = unpackedRoot / m_ref.worldDataFile();
    const LevelInfo* level = m_worldData.load(file) ? m_worldData.level(m_ref.name) : nullptr;
    if (level == nullptr) {
        log::warn("Level: no level {} in {}; the default light and camera range stand in",
                  m_ref.name, paths::utf8(file));
        return;
    }
    m_level = level;
    m_sim.lighting = WorldLighting::forLevel(*level);
    m_sim.litNow = m_sim.lighting;
    m_sim.ambientOffset = 0.0f;
    if (const LevelCameraInfo* camera = m_worldData.camera(level->cameraIndex); camera != nullptr) {
        m_cameraRange.radiusMin = camera->radiusMin;
        m_cameraRange.radiusMax = camera->radiusMax;
        m_cameraRange.minPitch = camera->minPitch;
        // camera_mode_level derives limits from the loaded world's header when
        // CAMS has no authored limits; its stored box is not used in that mode.
        m_cameraRange.boundsMin = camera->authoredBounds
                                      ? camera->boundsMin
                                      : m_layout.minBounds() + kCameraBoundsMinInset;
        m_cameraRange.boundsMax = camera->authoredBounds
                                      ? camera->boundsMax
                                      : m_layout.maxBounds() + kCameraBoundsMaxInset;
    }
    m_audio = m_worldData.audio(level->audioIndex);
}

void LevelWorld::clear() {
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_walls.clear();
    m_skorneArena.clear();
    m_fallingScenery.clear();
    m_scene.clear();
    m_worldAnimator.clear();
    m_destruction.clear();
    m_textureAnimator.clear();
    m_sim.particles.clear();
    m_sim.particlesAdvanced = false;
    m_triggers.clear();
    m_hazards.clear();
    m_rotators.clear();
    m_sim.rotatorCues.clear();
    m_movingObjects.clear();
    m_placedItems.clear();
    m_powerups.clear();
    m_collision.clear();
    m_markers.clear();
    m_textures.releaseTextures();
    m_itemArchives.clear();
    m_imported.reset();
    m_sim.frameRemainder = 0.0f;
    m_sim.textureAdvance = 0.0f;
    m_level = nullptr;
    m_audio = nullptr;
    m_sim.playerCount = -1;
}

std::optional<WorldCamera> LevelWorld::entranceCamera(u32 startIndex) const {
    const WorldLocator* locator =
        m_layout.findLocator(LocatorKind::CameraStart, isTower() ? startIndex : 0);
    if (locator == nullptr) {
        locator = m_layout.findLocator(LocatorKind::CameraStart);
    }
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

void LevelWorld::setPointLights(std::span<const PointLight> points) {
    m_sim.lighting.points.assign(points.begin(), points.end());
    m_sim.litNow.points = m_sim.lighting.points;
    m_scene.setPointLights(points);
}

bool LevelWorld::dark() const {
    return m_level != nullptr && (m_level->flags & DynamicLights::kDarkLevel) != 0;
}

void LevelWorld::setAmbientOffset(f32 offset) {
    // DoLighting pins the ambient special at minus one in a dark level (the Trenches, the
    // coin room), whatever a strike or a rite asks: only the lanterns light it.
    applyAmbientOffset(dark() ? -1.0f : offset);
}

void LevelWorld::applyAmbientOffset(f32 offset) {
    if (offset == m_sim.ambientOffset) {
        return;
    }
    m_sim.ambientOffset = offset;
    // A level's light saturates what faces it, so taking the offset off the ambient term
    // alone would leave most of it as bright as ever; the original darkens the whole picture
    // with it, and so is everything lit here: the level's baked geometry through the scene,
    // what is lit as it is drawn through the light it is given. What glows is left alone,
    // and the point lights add theirs over what is left.
    const f32 kept = std::clamp(1.0f + offset, 0.0f, 1.0f);
    m_sim.litNow = m_sim.lighting;
    m_sim.litNow.ambient = m_sim.lighting.ambient * kept;
    m_sim.litNow.lightColor = m_sim.lighting.lightColor * kept;
    m_sim.litNow.darken = 1.0f - kept; // the secret walls' and fixtures' baked colour as well
    m_scene.setDarken(1.0f - kept);
    m_scene.setLighting(m_sim.litNow);
}

const WorldLocator* LevelWorld::startPoint(u32 index) const {
    return m_layout.findLocator(LocatorKind::Start, index);
}

u32 LevelWorld::towerMarkerOf(u32 realm) {
    constexpr std::array<u32, 14> kMarkers{0, 3, 2, 6, 5, 9, 10, 1, 11, 7, 8, 4, 0, 0};
    return realm < kMarkers.size() ? kMarkers[realm] : 0;
}

const WorldLocator* LevelWorld::arrivalPoint(u32 realm, const TowerAccess* access) const {
    const u32 index =
        access != nullptr ? access->startMarker(towerMarkerOf(realm)) : towerMarkerOf(realm);
    const WorldLocator* marker = isTower() ? startPoint(index) : nullptr;
    return marker != nullptr ? marker : startPoint(0);
}

} // namespace gdl::game
