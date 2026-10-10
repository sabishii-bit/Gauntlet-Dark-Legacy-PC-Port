#pragma once

#include <map>

#include "game/netplay/CombatSnapshot.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {
class PlayerMissiles;
class EnemyMissiles;
class PlayerFigure;
class Enemies;
class LevelArrivalPresentation;
struct CombatantAssets;

/** Trusted load-time resource roster, not a packet-driven asset loader. The host
 * and client bind the same IDs to their own assets before play. Borrowed meshes,
 * textures and archives must outlive the roster. IDs cannot be rebound in place.
 * Models/effect meshes and streaks are covered; particles, lights, impact sounds
 * and gameplay callbacks are deliberately not reconstructed by this renderer. */
class ProjectileResources {
public:
    bool addModel(u32 id, const TreeModel& model);
    bool addStreak(u32 id, const Texture& texture);
    bool addEffect(u32 id, const EffectTrees::Effect& prototype, RenderDevice& device);
    bool addTree(u32 id, ItemArchive& archive, std::string_view tree, RenderDevice& device,
                 std::span<TextureSet* const> lenders = {});
    /** Load-time registration; does not start an effect or emit particles/sounds.
     * Names are sorted, and the entire family (including child attacks) commits
     * atomically. Both peers use the same firstId and texture lenders. */
    bool addCombatant(u32 firstId, CombatantAssets& stock, RenderDevice& device,
                      std::span<TextureSet* const> lenders = {});
    /** Preload shared weapons and each occupied seat's costume/SFX trees, including
     * future amulets, familiars and gauntlets. Call on both peers before loaded().
     * Reserves IDs 1..36863; other families must use higher IDs. Lenders must match
     * the scene's EffectTrees order. Failure preserves the previous registry. */
    bool addPlayers(RenderDevice& device, ItemArchive& weapons,
                    const std::array<PlayerFigure*, InputCommand::kSeats>& figures,
                    std::span<TextureSet* const> lenders = {});
    /** Preload the level's loaded swarm families, never its live instances.
     * IDs 36864..54271 depend on kind/tree index, not spawn/load order.
     * Call after both sides preload the same level roster; atomic on failure. */
    bool addEnemies(RenderDevice& device, Enemies& enemies,
                    std::span<TextureSet* const> lenders = {});
    /** Level, realm and common-powerup effects plus up to eight loaded fighter
     * families. IDs 54272..65535; families sort by native name, never load order.
     * Empty optional archives are allowed; repeated lenders/archives retain the
     * first binding. All owners must outlive the registry. Atomic on failure. */
    bool addStage(RenderDevice& device, const std::array<ItemArchive*, 3>& archives,
                  std::span<CombatantAssets* const> fighters,
                  std::span<TextureSet* const> lenders = {});
    u32 modelId(const TreeModel* model) const;
    u32 streakId(const Texture* texture) const;
    u32 effectId(const EffectTrees::Effect& effect) const;
    u32 treeId(const ItemArchive* archive, const TreeInfo* tree,
               std::span<TextureSet* const> lenders = {}) const;
    bool accepts(const ProjectileState& state) const;
    void draw(RenderDevice& device, const ProjectileState& state, const Mat4& clip,
              const WorldLighting& lighting, const CameraFrame& camera,
              TreeModel::Pass pass = TreeModel::Pass::All);
    void clear() { m_entries.clear(); }

private:
    bool addArchive(u32 firstId, ItemArchive& archive, RenderDevice& device,
                    std::span<TextureSet* const> lenders);
    struct Entry {
        const TreeModel* source = nullptr;
        const Texture* texture = nullptr;
        const ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        std::vector<TextureSet*> lenders;
        TreeModel model;
        TextureAnimator textures;
    };
    bool available(u32 id) const;
    std::map<u32, Entry> m_entries;
};

class ProjectileCapture {
public:
    /** Replaces the projectile roster atomically. Unregistered visible resources
     * or capacity overflow fail rather than silently omit a shot. Call alongside
     * CombatCapture at the same end-of-tick boundary; no gameplay queues drain. */
    static bool append(CombatSnapshot& snapshot, const ProjectileResources& resources,
                       const PlayerMissiles& players, const EnemyMissiles& enemies,
                       const EffectTrees* worldEffects = nullptr,
                       const LevelArrivalPresentation* arrival = nullptr);
};

class ReplicaProjectiles {
public:
    bool begin(u64 epoch);
    void clear();
    bool show(const CombatSnapshot& snapshot, const ProjectileResources& resources);
    void draw(RenderDevice& device, ProjectileResources& resources, const Mat4& clip,
              const WorldLighting& lighting, const CameraFrame& camera,
              TreeModel::Pass pass = TreeModel::Pass::All) const;
    usize count() const { return m_shots.size(); }

private:
    u64 m_epoch = 0;
    std::optional<u64> m_tick;
    std::vector<ProjectileState> m_shots;
};
} // namespace gdl::game
