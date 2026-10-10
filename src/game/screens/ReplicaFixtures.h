#pragma once

#include "game/enemies/CritterStatues.h"
#include "game/enemies/Generators.h"
#include "game/netplay/CombatSnapshot.h"
#include "game/world/Breakables.h"
#include "game/world/Chests.h"
#include "game/world/ExitPortals.h"
#include "game/world/LevelTriggers.h"
#include "game/world/LockedGates.h"
#include "game/world/Rubble.h"
#include "game/world/SafeRocks.h"
#include "game/world/Traps.h"

namespace gdl::game {
/** Trusted load-time fixture models. Includes every archive tree and loaded
 * generator damage state, so later snapshots never cause file or GPU loads.
 * Archives/generator bodies and their device resources must outlive this roster. */
class FixtureResources {
public:
    bool bind(RenderDevice& device, std::span<ItemArchive* const> archives,
              const Generators& generators, const SafeRocks& rocks,
              const CritterStatues* statues = nullptr);
    u32 id(const ItemFigure::Presentation& figure) const;
    u32 generatorId(s32 kind, s32 state) const;
    u32 rockId(usize index, s32 tier) const;
    u32 objectId(const ItemArchive* archive, std::string_view object) const;
    u32 portalId(const ExitPortals::Portal& portal) const;
    bool accepts(const FixtureState& fixture) const;
    void draw(RenderDevice& device, const FixtureState& fixture, const Mat4& clip,
              const WorldLighting& lighting, const CameraFrame& camera, TreeModel::Pass pass);

private:
    struct Entry {
        const ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        TreeModel model;
        TextureAnimator textures;
        std::optional<std::pair<s32, s32>> generator;
        std::optional<std::pair<usize, s32>> rock;
        std::unique_ptr<TreeInfo> staticTree;
        std::string object;
    };
    std::vector<Entry> m_entries;
};

class FixtureCapture {
public:
    struct Sources {
        const Chests& chests;
        const LockedGates& gates;
        const LevelTriggers& switches;
        const Generators& generators;
        const Breakables& barrels;
        const Traps& traps;
        const SafeRocks& rocks;
        const Rubble& rubble;
        const CritterStatues* statues = nullptr;
        const ExitPortals* portals = nullptr;
    };
    /** Atomic, read-only mesh capture. No unlocking, AI, spawning or animation
     * events; missing visible resources reject instead of silently dropping art. */
    static bool append(CombatSnapshot& snapshot, const FixtureResources& resources,
                       const Sources& sources);
};

/** Presentation only. Full snapshots remove absent fixtures; an open chest can
 * be reconstructed without receiving its earlier opening transition. */
class ReplicaFixtures {
public:
    bool begin(u64 epoch);
    void clear();
    bool show(const CombatSnapshot& snapshot, const FixtureResources& resources);
    void draw(RenderDevice& device, FixtureResources& resources, const Mat4& clip,
              const WorldLighting& lighting, const CameraFrame& camera,
              TreeModel::Pass pass = TreeModel::Pass::All) const;
    usize count() const { return m_fixtures.size(); }

private:
    u64 m_epoch = 0;
    std::optional<u64> m_tick;
    std::vector<FixtureState> m_fixtures;
};
} // namespace gdl::game
