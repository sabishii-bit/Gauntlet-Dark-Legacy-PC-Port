#pragma once

#include "game/config/GameConfig.h"
#include "game/netplay/MatchSession.h"
#include "game/screens/ReplicaActors.h"
#include "game/screens/ReplicaFighters.h"
#include "game/screens/ReplicaFixtures.h"
#include "game/screens/ReplicaHud.h"
#include "game/screens/ReplicaPickups.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/world/LevelWorld.h"

namespace gdl::game {
/** Development client view of the replicated combat slice. There is deliberately
 * no PlayScene, update(), input dispatch or persistence here. The trusted loading
 * step supplies all assets before show(); packets cannot open files or spawn AI.
 * Stage geometry is a separately owned renderer driven by host checkpoints.
 * Other overlays, sound and particles remain separate
 * work: this is not yet a replacement for the complete local scene renderer.
 * Borrowed world/resources must remain loaded for this view's lifetime. */
class ReplicaView {
public:
    bool begin(const MatchContext& context, const LevelWorld& world, Enemies& enemies,
               ProjectileResources& projectiles, PickupResources& pickups,
               FixtureResources& fixtures, FighterResources fighters = {});
    /** Same-scene pause resume retains loaded figures and borrowed projectile
     * resources. Keep displaying the paused image until a full new-epoch sample
     * arrives. Travel must instead release old registries before begin(). */
    bool resume(const MatchContext& context);
    void clear();
    bool setPlayer(u8 seat, std::unique_ptr<PlayerFigure> figure, f32 scale = 1,
                   const ClassStats* stats = nullptr);
    bool bindCompanions(RenderDevice& device, ItemArchive& powerups, ItemArchive* weapons);
    void loadPortalSkin(RenderDevice& device, TextureSet& weapons);
    bool loadHud(RenderDevice& device, const std::filesystem::path& root,
                 const StringTable* strings, const HudResources& resources = {});
    /** Takes a presentation sample, including fractional animation/motion. Missing
     * assets or stale/foreign epochs reject the whole sample, preserving the view. */
    bool show(const CombatSnapshot& snapshot);
    /** Empty when a sample passes preflight; static diagnostic, no mutation or loading. */
    std::string_view rejection(const CombatSnapshot& snapshot) const;
    void draw(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth, f32 frameHeight,
              f32 textureFrame, const Texture* hitFlash = nullptr, const Texture* frozen = nullptr,
              const GameConfig& video = {});
    static std::optional<Rect> viewport(f32 width, f32 height);
    const CombatSnapshot* shown() const { return m_shown ? &*m_shown : nullptr; }
    const ReplicaActors& actors() const { return m_actors; }
    usize projectileCount() const { return m_projectiles.count(); }
    usize pickupCount() const { return m_pickups.count(); }
    usize fixtureCount() const { return m_fixtures.count(); }
    usize fighterMeshCount() const { return m_fighters.count(); }
    const WorldScene& geometry() const { return m_geometry; }
    /** Facing direction from the last displayed camera and player, not a newer snapshot. */
    std::optional<Vec3> cursorAim(u8 seat, Vec2 cursor) const;

private:
    std::string_view resourceRejection(const CombatSnapshot& snapshot) const;
    MatchContext m_context;
    const LevelWorld* m_world = nullptr;
    Enemies* m_enemies = nullptr;
    ProjectileResources* m_resources = nullptr;
    PickupResources* m_pickupResources = nullptr;
    FixtureResources* m_fixtureResources = nullptr;
    ReplicaActors m_actors;
    std::array<f32, InputCommand::kSeats> m_playerHeights{};
    ReplicaProjectiles m_projectiles;
    ReplicaPickups m_pickups;
    ReplicaFixtures m_fixtures;
    FighterResources m_fighterResources;
    ReplicaFighters m_fighters;
    WorldScene m_geometry;
    ReplicaHud m_hud;
    std::optional<CombatSnapshot> m_shown;
    bool m_resuming = false;
    std::optional<Mat4> m_presentedClip;
    std::array<std::optional<Vec3>, InputCommand::kSeats> m_presentedPlayers;
};
} // namespace gdl::game
