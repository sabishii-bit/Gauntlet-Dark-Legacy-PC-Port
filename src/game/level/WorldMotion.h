#pragma once

#include "engine/world/SceneGeometry.h"

#include "game/level/LevelTriggers.h"
#include "game/level/Rotators.h"

namespace gdl::game {
/** Continuation of known world motion, not a checkpoint of triggers or world gameplay.
 * Authored tracks/targets are rebound locally. New switch, damage and contact events
 * are never invented; a new authoritative checkpoint replaces these clocks. */
class WorldMotion {
public:
    static constexpr usize kMaxObjects = 65535;
    static constexpr usize kMaxAnimations = 2048;
    static constexpr u8 kSolid = 1;
    static constexpr u8 kContactOnly = 2;
    static constexpr u8 kFloorExitBlocked = 4;
    static constexpr u8 kPlayerPassThrough = 8;
    struct State {
        std::vector<WorldAnimator::Cursor> animations;
        LevelTriggers::Motion triggers;
        std::vector<Rotators::Cursor> rotators;
        /** Every layout object, including invisible/static ones. No visual inference. */
        std::vector<u8> collision;
        bool pauseLoops = false;
        bool valid() const;
        bool operator==(const State&) const = default;
    };
    void bind(const WorldLayout& layout);
    /** Validate all trusted rosters before changing clocks, geometry or collision. */
    bool prepare(const State& state, const SceneGeometry& geometry, WorldScene& scene,
                 WorldCollision& collision);
    bool beforeMotion(f32 seconds, WorldScene& scene, WorldCollision& collision);
    bool afterMotion(f32 seconds, WorldScene& scene, WorldCollision& collision);
    State checkpoint(const WorldCollision& collision) const;
    static State capture(const WorldAnimator& animator, const LevelTriggers& triggers,
                         const Rotators& rotators, const WorldCollision& collision,
                         usize objectCount, bool pauseLoops = false);

private:
    void sync(WorldScene& scene, WorldCollision& collision) const;
    WorldAnimator m_animator;
    LevelTriggers m_triggers;
    Rotators m_rotators;
    std::vector<s32> m_moving;
    usize m_objects = 0;
    bool m_pauseLoops = false;
    bool m_prepared = false;
};
} // namespace gdl::game
