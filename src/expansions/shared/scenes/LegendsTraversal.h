#pragma once

#include <optional>

#include "engine/world/WorldCollision.h"

#include "expansions/shared/formats/LegendsMap.h"
#include "game/players/PlayerActor.h"

namespace gdl::game {

/** Revalidate imported scenery's horizontal correction through the common
 * player's static wall, step and floor rules. Refusal restores motion, never
 * inventory/health changes already made during the gameplay update. */
bool correctImportedSceneryStep(PlayerActor& actor, const PlayerActor& before,
                                const WorldCollision& collision, const Vec3& correctedPosition,
                                f32 seconds);

/** Native Legends collision with the port's player locomotion. Owns the visiting
 * character's runtime copy; the application controls return and save boundaries. */
class LegendsTraversal {
public:
    /** Throws FormatError if no safe starting floor can be found. */
    void open(const formats::LegendsMap& map, s32 player, const CharacterSave& save,
              const ClassStats* stats = nullptr);
    void step(const MoveInput& input, f32 cameraYaw, f32 seconds,
              const std::optional<Vec3>& aim = {}, bool freeMove = false);
    /** Revalidates a scenery-adjusted horizontal step through the same native
     * player movement queries. `before` is the actor copied immediately before
     * step(), and seconds is that step's duration. On refusal its complete body
     * state is restored, retaining the new facing. Never places through walls
     * or settles onto an otherwise unreachable floor. */
    bool correctSceneryStep(const PlayerActor& before, const Vec3& correctedPosition, f32 seconds);
    const PlayerActor& actor() const { return m_actor; }
    PlayerActor& actor() { return m_actor; }
    const WorldCollision& collision() const { return m_collision; }
    bool authoredStart() const { return m_authoredStart; }
    const Vec3& start() const { return m_start; }

private:
    WorldCollision m_collision;
    PlayerActor m_actor;
    Vec3 m_start{0};
    bool m_authoredStart = false;
};

} // namespace gdl::game
