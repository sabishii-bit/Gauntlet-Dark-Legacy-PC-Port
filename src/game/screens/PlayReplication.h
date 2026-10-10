#pragma once

#include "game/netplay/MatchSession.h"
#include "game/screens/PlayScene.h"
#include "game/screens/ReplicaFighters.h"
#include "game/screens/ReplicaFixtures.h"
#include "game/screens/ReplicaPickups.h"
#include "game/screens/ReplicaProjectiles.h"
#include "game/screens/SessionInputs.h"

namespace gdl::game {
/** Host-only fixed-step bridge into the real scene. Bind after loading each match
 * epoch, before loaded(). Does not load characters, write saves or navigate menus.
 * The borrowed scene/resources must outlive this bridge. Capture failure latches:
 * callers must leave the match, never continue with partially replicated state.
 * Current output covers the CombatSnapshot slice, not complete level replication. */
class PlayReplication {
public:
    enum class Result : u8 { Held, Advanced, Travel, GameOver, Leave, Failed };
    static constexpr u64 kSnapshotStride = 3;
    bool bind(PlayScene& scene, const MatchSession& match, const PickupResources& pickups,
              const FixtureResources& fixtures);
    void clear();
    /** Exactly once per 60 Hz application simulation tick, never per rendered frame. */
    Result advance(MatchSession& match, const SessionInputs::Frame& local,
                   const ProjectileResources& resources);
    const CombatSnapshot* latest() const { return m_latest ? &*m_latest : nullptr; }
    bool failed() const { return m_failed; }

private:
    Result fail();
    bool rosterMatches(const MatchContext& context) const;
    void filterArrivalSkip(SessionInputs::Frame& inputs);
    std::optional<CombatSnapshot> capture(u64 tick, const ProjectileResources& resources);
    PlayScene* m_scene = nullptr;
    const PickupResources* m_pickups = nullptr;
    const FixtureResources* m_fixtures = nullptr;
    FighterResources m_fighters;
    MatchContext m_context;
    std::optional<CombatSnapshot> m_latest;
    std::array<u32, InputCommand::kSeats> m_continuity{};
    std::array<bool, InputCommand::kSeats> m_arrivalSkipVotes{};
    u32 m_cameraContinuity = 1;
    u64 m_nextTick = 0;
    bool m_finished = false;
    bool m_failed = false;
    bool m_quiet = false;
};
} // namespace gdl::game
