#pragma once

#include <memory>
#include <vector>

#include "engine/core/Types.h"

#include "game/level/ItemFigure.h"
#include "game/level/ItemSupport.h"

namespace gdl::game {
/** Ordinary type-10/subtype-0 item scenery, including the level's exit sign.
 * These are solid placed items, not exit triggers or breakable scenery. */
class StaticScenery {
public:
    StaticScenery() = default;
    StaticScenery(const StaticScenery&) = delete;
    StaticScenery& operator=(const StaticScenery&) = delete;
    StaticScenery(StaticScenery&&) noexcept = default;
    StaticScenery& operator=(StaticScenery&&) noexcept = default;
    ~StaticScenery() = default;
    struct Prop {
        s32 instance = -1;
        s32 minPlayers = 1;
        bool shown = true;
        ItemFigure figure;
        ItemSupport support;
        Obstacle box;
        bool operator==(const Prop&) const = default;
        template <class Visitor> friend void visitNativeState(Visitor& visitor, const Prop& value) {
            visitor.template fields<6>(value);
        }
    };
    void bind(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope,
              const WorldCollision* collision);
    void clear();
    void setPlayerCount(s32 players);
    void capturePresentation();
    void update(f32 seconds);
    void syncFloors();
    std::vector<Obstacle> obstacles() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera, TreeModel::Pass pass, f32 presentationAlpha) const;
    usize size() const { return m_sim.props.size(); }
    const Prop& prop(usize index) const { return m_sim.props[index]; }

private:
    struct Simulation {
        std::vector<Prop> props;
        bool operator==(const Simulation&) const = default;
    } m_sim;

public:
    /** In-memory history for this owner and its current bound stage. */
    class LocalSnapshot {
        friend class StaticScenery;
        Simulation m_state;
        std::weak_ptr<const u8> m_binding;
        const StaticScenery* m_owner = nullptr;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    bool registerNativeResources(NativeStateResources& resources) const;

private:
    bool appendSimulationDigest(NativeStateDigest& digest, const Simulation& state) const;
    const WorldCollision* m_collision = nullptr;
    const WorldLayout* m_layout = nullptr;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
    u64 m_checkpointGeneration = 0;
};
} // namespace gdl::game
