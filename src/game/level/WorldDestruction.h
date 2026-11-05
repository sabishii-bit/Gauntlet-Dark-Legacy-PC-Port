#pragma once

#include <memory>
#include <span>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"

namespace gdl::game {

/** WorldObjectExplode / DoWorldAnimSub: carts and explosive scenery disappear along
 * their parent chain. A looping cart reappears at the start of its next pass. */
class WorldDestruction {
private:
    struct Simulation {
        std::vector<bool> destroyed;
        std::vector<Vec3> explosions;
        bool operator==(const Simulation&) const = default;
        template <class Digest>
        friend void visitNativeState(Digest& digest, const Simulation& value) {
            digest.template fields<2>(value);
        }
    };

public:
    class LocalSnapshot {
        friend class WorldDestruction;
        Simulation m_state;
        std::weak_ptr<const u8> m_binding;
        const WorldDestruction* m_owner = nullptr;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

    void bind(const WorldLayout& layout);
    void clear();
    std::span<const usize> controlledObjects() const { return m_controlled; }
    bool destroyed(s32 object) const;
    /** Exact native ancestor flag/previous-explosion test, without changing state. */
    bool wouldExplode(s32 object) const;
    bool explode(s32 object, const Vec3& position, WorldScene& scene, WorldCollision& collision);
    void update(std::span<const WorldAnimator::CycleEvent> events, WorldScene& scene,
                WorldCollision& collision);
    std::vector<Vec3> takeExplosions();
    bool hasExplosions() const { return !m_sim.explosions.empty(); }

private:
    bool appendDigest(NativeStateDigest& digest, const Simulation& state) const;
    bool valid(s32 object) const;
    bool below(usize descendant, s32 ancestor) const;
    void showSubtree(s32 object, bool visible, WorldScene& scene);
    std::vector<WorldObject> m_objects;
    Simulation m_sim;
    std::vector<usize> m_controlled;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
};
} // namespace gdl::game
