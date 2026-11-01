#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/world/TextureAnimator.h"

#include "expansions/n64/bosses/LegendsBosses.h"
#include "expansions/n64/critters/LegendsCritters.h"
#include "expansions/n64/enemies/LegendsDeath.h"
#include "expansions/n64/enemies/LegendsGenerators.h"
#include "expansions/n64/formats/LegendsN64Camera.h"
#include "expansions/n64/formats/LegendsN64Progress.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/level/LegendsBossEruption.h"
#include "expansions/n64/level/LegendsContainers.h"
#include "expansions/n64/level/LegendsFixtureEvents.h"
#include "expansions/n64/level/LegendsGates.h"
#include "expansions/n64/level/LegendsLootSelectors.h"
#include "expansions/n64/level/LegendsObelisk.h"
#include "expansions/n64/level/LegendsPortals.h"
#include "expansions/n64/level/LegendsScenery.h"
#include "expansions/n64/level/LegendsTowerMotion.h"
#include "expansions/n64/level/LegendsTowerPortals.h"
#include "expansions/n64/level/LegendsTowerProgress.h"
#include "expansions/n64/level/LegendsTraps.h"
#include "expansions/n64/level/LegendsUnlocks.h"
#include "expansions/n64/level/LegendsWorldMotion.h"
#include "expansions/shared/formats/LegendsMap.h"
#include "expansions/shared/level/LegendsItems.h"
#include "expansions/shared/scenes/LegendsMapCatalog.h"
#include "game/critters/CombatEvents.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/level/ImportedWorldObject.h"
#include "game/level/ItemFigure.h"
#include "game/players/PlayerActor.h"

namespace gdl::game {

/** Native Legends presentation bound to the common item/texture animation runtime.
 * Owns its archives; no campaign, player inventory or save state is borrowed. */
// Its members stand in the order the level is bound, subject by subject; packing them
// would scatter that reading for a few dozen bytes of padding on one object.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class LegendsSceneAdapter {
public:
    static std::unique_ptr<LegendsSceneAdapter> load(RenderDevice& device, LegendsPlatform platform,
                                                     const std::filesystem::path& directory,
                                                     formats::LegendsMap& map, s32 playerCount = 1);
    void update(f32 seconds);
    void capturePresentation();
    void draw(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
              TreeModel::Pass pass, f32 presentationAlpha = -1) const;
    usize fixtureCount() const { return m_fixtures.size(); }
    /** Console-only identity range, separate from native world and combat IDs.
     * Animated variants of one fixture retain the same identity. */
    static constexpr s32 kFixtureObjectBase = 0x01000000;
    std::optional<ImportedWorldObject> fixtureObject(s32 id) const;
    std::optional<ImportedObjectHit> pickFixture(const Vec3& origin, const Vec3& direction,
                                                 const CameraFrame& camera, f32 alpha) const;
    void highlightFixture(RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                          f32 alpha, s32 id, const SelectionHighlight& highlight) const;
    usize textureAnimationCount() const { return m_worldMotion.size() + m_nativeScrolls.size(); }
    const std::vector<std::string>& warnings() const { return m_warnings; }
    LegendsPlatform platform() const { return m_platform; }
    bool isTower() const { return m_gatedPortals; }
    const std::optional<formats::LegendsN64CameraData>& cameraConfig() const { return m_camera; }
    const std::optional<LevelCameraInfo>& dreamcastCameraConfig() const {
        return m_dreamcastCamera;
    }
    std::span<const WorldLocator> dreamcastCameraMarkers() const {
        return m_dreamcastCameraMarkers;
    }
    std::span<const LegendsPickup> pickups() const { return m_pickups; }
    std::span<const ImportedGenerator> generators() const { return m_generators; }
    std::span<const ImportedActor> actors() const { return m_actors; }
    ItemArchive* actorArchive(s32 kind) const;
    void spawnBossRewards(Vec3 death, Vec3 start);
    void updateBossRewards(f32 seconds, const WorldCollision& collision);
    std::vector<CombatArenaTarget> arenaTargets() const;
    std::vector<Mat4> arenaAnchors() const;
    std::vector<Obstacle> arenaObstacles() const;
    std::vector<MissileStop> arenaMissileStops() const;
    bool arenaBlocksSegment(const Vec3& from, const Vec3& to, f32 radius) const;
    bool strikeArenaRock(usize index, f32 power);
    void scheduleArenaActivation(usize index, f32 delay);
    void beginBossEntrance();
    /** Mirror common GDL health states into prebound native artwork. No GPU work. */
    void updateGenerators(std::span<const ImportedGeneratorState> states);
    std::span<const formats::LegendsN64Portal> portals() const { return m_portals; }
    bool portalUnlocked(usize index) const {
        return index < m_portalPresentation.size() && m_portalPresentation[index].enabled;
    }
    /** Call once per gameplay tick after contact/movement. A destination is
     * emitted once; the caller runs GDL departure/loading before replacing us. */
    std::optional<std::string> tickPortals(std::span<const LegendsPortalVisitor> players,
                                           f32 seconds);
    void cancelPortalTravel();
    std::optional<LegendsTowerPortalKind> towerDeparture() const;
    std::vector<PointLight> towerPortalLights() const;
    /** Script-created scenery volumes are absent from the static collision mesh. */
    Vec3 resolveMovement(const Vec3& from, const Vec3& to, f32 radius, f32 height) const;
    std::vector<LegendsContainerTarget> containerTargets() const;
    std::vector<Obstacle> containerObstacles() const;
    bool strikeContainer(s32 localId, f32 power, u32 flags, s32 byPlayer);
    std::vector<ImportedTarget> deathTargets() const;
    std::vector<Obstacle> deathObstacles() const;
    bool strikeDeath(s32 localId, f32 power);
    void touchDeaths(Vec3 feet, f32 radius, f32 height);
    void releaseDeaths(const std::function<bool(const EnemySpawn&)>& release);
    bool takeDeathAdvice();
    std::vector<ImportedTarget> sceneryTargets() const { return m_scenery.targets(); }
    bool strikeScenery(s32 localId, f32 power, u32 flags);
    bool strikeWorldObject(s32 object, f32 power, u32 flags);
    bool worldObjectVisible(usize object) const;
    f32 worldObjectWhiteBlend(usize object) const { return m_scenery.whiteBlend(object); }
    f32 worldObjectAlpha(usize object, f32 alpha = -1) const {
        return m_worldMovement.opacity(object, alpha);
    }
    void supportSwitches(std::span<const s32> owners) { m_worldMovement.support(owners); }
    Vec3 worldObjectOffset(usize object, f32 alpha = -1) const {
        return m_worldMovement.offset(object, alpha) + m_bossEruption.offset(object, alpha);
    }
    Mat4 worldObjectTransform(usize object, f32 alpha = -1) const {
        return glm::translate(Mat4{1}, m_worldMovement.offset(object, alpha)) *
               m_bossEruption.transform(object, alpha);
    }
    void bindCollision(WorldCollision& collision) const {
        m_worldMovement.bindCollision(collision);
        m_scenery.syncCollision(collision);
    }
    void syncCollision(WorldCollision& collision) const {
        m_worldMovement.syncCollision(collision);
        m_scenery.syncCollision(collision);
    }
    void touchSwitches(std::span<const LegendsPortalVisitor> visitors);
    void updateTraps(f32 seconds, bool timeStopped);
    void touchTraps(std::span<const LegendsPortalVisitor> visitors);
    void setTrapView(const ViewVolume& view);
    std::vector<LegendsTrapHit> takeTrapHits();
    std::vector<LegendsFixtureEvent> takeFixtureEvents();
    std::vector<LegendsSoundEvent> takeSounds();
    std::vector<u32> takeObeliskSpeech();
    std::vector<LegendsSoundLoop> soundLoops() const { return m_worldMovement.soundLoops(); }
    /** Supply the active party before first presentation and after shared awards. */
    void synchronizeProgress(std::span<const CharacterSave* const> party);
    using PickupObserver = std::function<void(const LegendsPickup&, const ItemTaking&)>;
    /** Collect using GDL reach and inventory rules; returns successfully taken names.
     * The optional synchronous observer receives each completed grant for common feedback. */
    std::vector<std::string> collect(PlayerActor& actor, f32 powerupTime, bool activatePowerups,
                                     const PickupObserver& observer = {},
                                     std::span<CharacterSave* const> party = {});

    struct TextureView {
        const Texture* texture = nullptr;
        Vec2 offset{0};
        Vec2 scale{1};
        bool hasTransparency = false;
        bool softAlpha = false;
    };
    TextureView texture(u32 slot) const;

private:
    std::optional<usize>
    bindN64Death(const formats::LegendsN64Fixture& fixture, usize figure,
                 std::span<const formats::LegendsMapCollisionTriangle> collision);
    void updateDeaths();
    struct DeathPresentation {
        LegendsDeath state;
        usize figure = 0;
        Mat4 initialTransform{1};
        s32 floorOwner = -1;
        bool active = true;
    };
    std::vector<DeathPresentation> m_deaths;
    bool fixtureVisible(usize index) const;
    usize fixtureIdentity(usize index) const;
    void drawFixture(usize index, RenderDevice& device, const Mat4& clip, const CameraFrame& camera,
                     TreeModel::Pass pass, f32 alpha) const;
    void bindN64Trap(RenderDevice& device, formats::LegendsN64Trap definition, usize idleFigure,
                     f32 damageScale, s32 floorOwner = -1);
    void loadDreamcast(RenderDevice& device, const std::filesystem::path& directory);
    void loadN64(RenderDevice& device, const std::filesystem::path& directory,
                 std::span<const formats::LegendsMapCollisionTriangle> collision);
    void loadN64Tower(RenderDevice& device, const std::filesystem::path& root,
                      std::span<const u8> program);
    void updateTower();
    void updateTowerPortals();
    void bindN64BossRewards(RenderDevice& device, const LegendsBossEncounter& encounter,
                            std::span<const formats::LegendsN64Fixture> fixtures);
    void bindN64Arena(RenderDevice& device, const ImportedBoss& boss);
    void updateArena(f32 seconds);
    void selectArenaRock(usize index, bool burst = false);
    void updateTowerProgress(bool initial);
    void bindN64Gate(RenderDevice& device, formats::LegendsN64Gate definition, usize idleFigure);
    void touchGates(PlayerActor& actor);
    void updateGates();
    void bindN64Obelisk(RenderDevice& device, const formats::LegendsN64Obelisk& definition,
                        usize idleFigure);
    void updateObelisks(f32 seconds = 0);
    void bindN64ObeliskEffects(RenderDevice& device, const std::filesystem::path& root);
    f32 obeliskWhiteBlend(usize figure) const;
    void drawObeliskEffects(RenderDevice& device, const Mat4& clip,
                            const CameraFrame& camera) const;
    void bindN64Rider(const formats::LegendsN64Fixture& fixture,
                      std::span<const formats::LegendsMapCollisionTriangle> collision,
                      usize firstFigure, usize firstPickup, usize firstContainer, usize firstGate);
    void updateRiders();
    void bindN64Switch(RenderDevice& device, const formats::LegendsN64Switch& definition,
                       usize idleFigure);
    void bindWorldMotion(const formats::LegendsMap& map,
                         std::span<const formats::LegendsN64Switch> definitions);
    void updateSwitches();
    void bindN64Generator(RenderDevice& device, const formats::LegendsN64Fixture& fixture,
                          std::span<const u8> templates, std::string_view directory,
                          std::span<const formats::LegendsMapCollisionTriangle> collision,
                          usize idleFigure);
    void bindN64Container(RenderDevice& device, LegendsContainerDefinition definition,
                          usize idleFigure);
    void bindN64Selectors(std::span<const u8> powerups, std::span<const u8> randoms,
                          std::span<const formats::LegendsN64Fixture> fixtures);
    void bindN64FloorSelector(RenderDevice& device, const formats::LegendsN64Fixture& fixture,
                              usize idleFigure);
    void updateContainerSelectors(f32 seconds);
    void prepareSelectorPickup(usize pickup);
    void selectFloorOutcome(usize index);
    void openContainer(usize index);
    void updateContainers();
    void touchContainers(PlayerActor& actor);
    void bindPortal(RenderDevice& device, formats::LegendsN64Portal portal, usize idleFigure);
    void updatePortals();
    void selectPortal(usize index);
    void bindWorld(RenderDevice& device, const formats::LegendsMap& map,
                   std::span<const TextureAnimationInfo> animations);
    void place(RenderDevice& device, std::string_view name, const ItemInstance& instance,
               f32 scale = 1, u32 objectFlags = 0);
    // Figures and animation bindings must be destroyed before their borrowed archives.
    std::vector<std::unique_ptr<ItemArchive>> m_archives;
    TextureSet m_worldTextures;
    std::vector<TextureView> m_worldViews;
    struct WorldBinding {
        TextureSet* bank = nullptr;
        u32 slot = 0;
    };
    std::vector<WorldBinding> m_worldBindings;
    struct ArchiveMotion {
        TextureSet* bank = nullptr;
        TextureAnimator animation;
    };
    std::vector<ArchiveMotion> m_archiveMotion;
    std::vector<TextureView> m_coverage;
    TextureAnimator m_worldMotion;
    struct NativeScroll {
        formats::LegendsN64TextureScroll definition;
        formats::LegendsN64TextureScrollState state;
    };
    std::vector<NativeScroll> m_nativeScrolls;
    std::vector<ItemFigure> m_fixtures;
    std::vector<bool> m_hiddenFixtures;
    std::vector<bool> m_heldFixtures;
    std::vector<f32> m_fixtureOpacity;
    mutable std::vector<usize> m_fixtureIdentities;
    struct TrapPresentation {
        LegendsTrap state;
        std::array<std::optional<usize>, 4> figures;
        u32 generation = 0;
        s32 floorOwner = -1;
        Vec3 basePosition{0};
    };
    std::vector<TrapPresentation> m_traps;
    std::vector<LegendsTrapHit> m_trapHits;
    std::optional<ViewVolume> m_trapView;
    std::vector<LegendsFixtureEvent> m_fixtureEvents;
    std::vector<LegendsSoundEvent> m_soundEvents;
    struct GatePresentation {
        LegendsGate state;
        usize idleFigure = 0;
        usize openingFigure = 0;
    };
    std::vector<GatePresentation> m_gates;
    struct ObeliskPresentation {
        usize pickup = 0;
        usize idleFigure = 0;
        usize spentFigure = 0;
        LegendsObeliskMotion motion;
        u32 sound = 0;
        u32 identity = 0;
    };
    std::vector<ObeliskPresentation> m_obelisks;
    std::unique_ptr<Texture> m_obeliskSparkle;
    std::vector<u32> m_obeliskSpeechEvents;
    bool m_deathAdvicePending = false;
    LegendsProgress m_partyProgress;
    bool m_progressInitialized = false;
    struct NativeRider {
        struct Figure {
            usize index;
            Mat4 placement;
        };
        struct Point {
            usize index;
            Vec3 position;
        };
        s32 owner = -1;
        Vec3 appliedOffset{0};
        std::vector<Figure> figures;
        std::vector<Point> pickups, containers, gates;
    };
    std::vector<NativeRider> m_nativeRiders;
    LegendsWorldMotion m_worldMovement;
    LegendsScenery m_scenery;
    struct SwitchPresentation {
        u32 nativeIndex = 0;
        usize idleFigure = 0;
        std::optional<usize> activeFigure;
        bool activated = false;
        f32 yaw = 0;
        f32 scale = 1;
        u32 sound = 0;
    };
    std::vector<SwitchPresentation> m_switchPresentation;
    struct GeneratorPresentation {
        u32 nativeIndex = 0;
        usize originalFigure = 0;
        std::array<std::optional<usize>, 4> figures;
        std::optional<usize> active;
        bool installed = false;
        f32 yaw = 0;
    };
    std::vector<ImportedGenerator> m_generators;
    std::vector<ImportedActor> m_actors;
    ItemArchive* m_itArchive = nullptr; ///< borrowed from m_archives, never shared across levels
    struct BossRewardPresentation {
        LegendsBossReward definition;
        usize pickup = 0;
        usize figure = 0;
        LegendsBossRewardMotion motion;
        bool flying = false;
        bool released = false;
    };
    std::vector<BossRewardPresentation> m_bossRewards;
    bool m_bossRewardsReleased = false;
    struct ArenaRockPresentation {
        ImportedBoss::ArenaRock definition;
        std::array<std::optional<usize>, 4> figures;
        std::optional<usize> burstFigure, activeFigure;
        s32 health = 0;
        s32 tier = 3;
        bool dormant = false;
        f32 activationDelay = 0;
    };
    std::vector<ArenaRockPresentation> m_arenaRocks;
    bool m_hasBossEntrance = false;
    LegendsBossEruption m_bossEruption;
    std::vector<GeneratorPresentation> m_generatorPresentation;
    struct ContainerPresentation {
        LegendsContainer state;
        usize closedFigure = 0;
        usize openingFigure = 0;
        usize brokenFigure = 0;
        std::optional<usize> pickup;
        struct Outcome {
            u16 key = 0;
            usize openingFigure = 0;
            std::optional<usize> pickup;
            std::optional<usize> death;
        };
        std::optional<usize> selector;
        std::vector<Outcome> outcomes;
        std::optional<usize> blastFigure;
        f32 blastSeconds = 0;
        std::optional<usize> death;
    };
    std::vector<ContainerPresentation> m_containers;
    LegendsLootSelectors m_lootSelectors;
    struct FloorSelectorPresentation {
        usize slot = 0;
        usize pickup = 0;
        std::optional<usize> activeFigure;
        u16 selected = 0;
        struct Outcome {
            u16 key = 0;
            std::optional<usize> figure;
            std::optional<LegendsPickup> pickup;
            std::optional<usize> death;
        };
        std::vector<Outcome> outcomes;
    };
    std::vector<FloorSelectorPresentation> m_floorSelectors;
    struct PortalPresentation {
        // idle, starting, active, closing; missing transitional art uses idle/active.
        std::array<usize, 4> figures{};
        usize active = 0;
        u32 generation = 0;
        LegendsPortalMotion motion;
        bool enabled = false;
        std::vector<usize> lockedMeshes;
    };
    LegendsN64Unlocks m_stageUnlocks;
    bool m_gatedPortals = false;
    std::vector<formats::LegendsN64Portal> m_portals;
    std::vector<PortalPresentation> m_portalPresentation;
    bool m_portalDeparture = false;
    bool m_waitForPortalExit = false;
    std::array<LegendsTowerPortalMotion, 2> m_towerPortals{
        LegendsTowerPortalMotion{LegendsTowerPortalKind::Shards},
        LegendsTowerPortalMotion{LegendsTowerPortalKind::Runestones}};
    struct TowerPresentation {
        struct Variant {
            std::string sequence;
            usize figure = 0;
        };
        std::vector<Variant> sumner;
        std::vector<Variant> podium;
        usize activeSumner = 0;
        usize activePodium = 0;
        LegendsTowerIdle idle;
        std::minstd_rand random{1};
    };
    std::optional<TowerPresentation> m_tower;
    struct TowerDoorPresentation {
        formats::LegendsN64TowerDoor definition;
        usize openingFigure = 0;
        usize openFigure = 0;
        LegendsTowerDoorMotion motion;
    };
    std::vector<TowerDoorPresentation> m_towerDoors;
    std::array<std::optional<usize>, 13> m_towerRunestones;
    std::array<std::optional<usize>, 4> m_towerShards;
    std::optional<usize> m_towerShardPortal;
    std::vector<usize> m_towerPortalHiddenMeshes;
    std::optional<usize> m_towerRunePortal;
    std::optional<usize> m_cathedralBeam;
    std::vector<formats::LegendsN64TowerVolume> m_towerResidentVolumes;
    std::vector<std::string> m_initiallyHiddenWorldObjects;
    std::vector<bool> m_hiddenWorldObjects;
    LegendsPlatform m_platform = LegendsPlatform::N64;
    s32 m_playerCount = 1;
    std::optional<formats::LegendsN64CameraData> m_camera;
    std::optional<LevelCameraInfo> m_dreamcastCamera;
    std::vector<WorldLocator> m_dreamcastCameraMarkers;
    std::vector<LegendsPickup> m_pickups;
    std::vector<usize> m_pickupFigures;
    std::vector<std::string> m_missing;
    std::vector<std::string> m_warnings;
};

} // namespace gdl::game
