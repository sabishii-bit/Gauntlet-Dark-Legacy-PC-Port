#pragma once

#include <vector>

#include "engine/core/LocalBinding.h"
#include "engine/core/Types.h"
#include "engine/io/LocalRoster.h"
#include "engine/io/LocalState.h"
#include "engine/io/NativeStateDigest.h"

#include "game/level/ItemFigure.h"
#include "game/level/ItemSupport.h"

namespace gdl::game {
/** Ordinary type-10/subtype-0 item scenery, including the level's exit sign.
 * These are solid placed items, not exit triggers or breakable scenery. */
class StaticScenery : public LocalState<StaticScenery> {
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
    bool registerNativeResources(NativeStateResources& resources) const;

private:
    struct Simulation {
        std::vector<Prop> props;
        bool operator==(const Simulation&) const = default;
    } m_sim;

    friend class LocalState<StaticScenery>;
    static constexpr LocalTraits kLocalTraits{.refusalInvalidates = true,
                                              .gatesOnBound = false,
                                              .gatesOnValues = false,
                                              .notifiesRestore = false};
    using LocalValues = Simulation;
    struct LocalChildren {};
    static constexpr auto localParts() { return local::parts(local::own()); }
    const LocalValues& localValues() const { return m_sim; }
    LocalValues& localValues() { return m_sim; }
    /** Writes the schema, what the scenery was bound to and `state`. */
    bool digestLocalValues(NativeStateDigest& digest, const LocalValues& state) const;
    const WorldCollision* m_collision = nullptr;
    const WorldLayout* m_layout = nullptr;
    LocalBinding m_localBinding;
};
} // namespace gdl::game
