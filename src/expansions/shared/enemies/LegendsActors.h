#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "game/level/ImportedActors.h"
#include "game/level/ItemFigure.h"

namespace gdl::game {
class Enemies;

/** Pending native actors are visible but have no collision, targets or combat
 * slots. Proximity replaces them once, only when the existing combat pool
 * accepts the spawn. All borrowed archives must outlive this owner. */
class LegendsActors {
public:
    using ArchiveFor = std::function<ItemArchive*(const ImportedActor&)>;
    using Spawn = std::function<bool(const ImportedActor&)>;

    void open(RenderDevice& device, std::span<const ImportedActor> actors,
              const ArchiveFor& archiveFor, const WorldCollision* collision);
    void clear();
    void syncFloors();
    void update(f32 seconds, std::span<const EnemyView> players, bool timeStopped,
                const Spawn& spawn);
    /** Only native It needs a live presentation supplement: the GC archive has
     * no body. Its common enemy slot still owns all movement and tagging. */
    void bindLive(u32 nativeIndex, s32 slot, u64 instance);
    /** After the common swarm step (or its floor carry), follow the same lifetime
     * and presentation clock; never bind a recycled slot's replacement. */
    void syncLive(const Enemies& enemies);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr, TreeModel::Pass pass = TreeModel::Pass::All,
              f32 presentationAlpha = -1) const;
    usize count() const { return m_actors.size(); }
    bool pending(usize index) const { return m_actors.at(index).pending; }
    const ImportedActor& descriptor(usize index) const { return m_actors.at(index).descriptor; }
    const ItemFigure& figure(usize index) const { return m_actors.at(index).figure; }
    const std::vector<std::string>& warnings() const { return m_warnings; }

private:
    struct Actor {
        ImportedActor descriptor;
        ItemFigure figure;
        Vec3 initialPosition{0};
        bool pending = true;
        s32 liveSlot = -1;
        u64 liveInstance = 0;
    };
    const WorldCollision* m_collision = nullptr;
    std::vector<Actor> m_actors;
    std::vector<std::string> m_warnings;
};

} // namespace gdl::game
