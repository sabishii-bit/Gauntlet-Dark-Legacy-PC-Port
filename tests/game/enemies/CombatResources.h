#pragma once

#include <memory>
#include <span>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/ParticleSystem.h"

#include "game/characters/ClassData.h"
#include "game/effects/EffectTrees.h"
#include "game/level/LevelWorld.h"
#include "game/party/PlayerArsenal.h"
#include "game/party/PlayerMissiles.h"
#include "game/party/PlayerRuntime.h"

namespace gdl::game {
class LevelOpponents;

/** Trusted, load-time bindings for a complete combat checkpoint. IDs describe
 * authored archives, lender contexts, trees and selected player seats, never
 * live effect handles or pointer enumeration. Unknown resources refuse the
 * enclosing checkpoint; this catalog does not authorize their simulation. */
class CombatResources {
public:
    /** What the catalog binds, as the pools take it: missiles, their effects, shared effects. */
    struct Resources {
        std::span<const PlayerMissiles::Resource> missiles;
        std::span<const EffectTrees::PoolResource> missileEffects;
        std::span<const EffectTrees::PoolResource> sharedEffects;
    };
    struct Bank {
        u32 id = 0;
        ItemArchive* archive = nullptr;
        std::vector<TextureSet*> lenders;
    };
    struct Particle {
        u32 id = 0; ///< trusted SFXX row identity, independent of live emission order
        u32 bank = 0;
        ParticleDescriptor descriptor;
        u32 texture = 0;
    };
    CombatResources();
    ~CombatResources();
    CombatResources(const CombatResources&) = delete;
    CombatResources& operator=(const CombatResources&) = delete;
    CombatResources(CombatResources&&) = delete;
    CombatResources& operator=(CombatResources&&) = delete;

    /** All declarations originate in local authenticated stage assets. This is
     * not a packet-driven loader. Copies retain immutable artwork pointers, so
     * clear all borrowing combat owners before releasing/replacing the catalog
     * or its archives. Preparation may bind/upload artwork; lookups never do.
     * Failure preserves the old catalog and does not touch any gameplay pool. */
    bool bind(RenderDevice& device, std::span<const Bank> banks,
              std::span<const PlayerMissiles::Resource> missiles = {},
              std::span<const Particle> particles = {});
    /** Both peers call after StageResources preloading, with the same selected
     * native profiles and scene lender order. Covers shared/stage/swarm/class
     * trees, native SFXX particles, ordinary weapon, familiar/Phoenix and potion models.
     * Unknown special missile specs and code-attached trail topologies remain
     * explicit whole-checkpoint refusals, rather than omitted state. */
    bool bindStage(RenderDevice& device, LevelWorld& world, ItemArchive& weapons,
                   const ClassDataSet& classes, std::span<PlayerRuntime> party,
                   const PlayerArsenal& arsenal, LevelOpponents& opponents,
                   const EffectTrees& effects);
    void clear();
    Resources resources() const;
    /** Figure replacement (for example a native promotion) requires a fresh
     * load-time catalog. Check before using retained player-resource pointers. */
    bool matches(std::span<const PlayerRuntime> party) const;
    usize bankCount() const;
    usize effectCount() const;

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
} // namespace gdl::game
