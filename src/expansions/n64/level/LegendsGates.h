#pragma once

#include <array>
#include <limits>
#include <optional>

#include "expansions/n64/formats/LegendsN64Gates.h"
#include "expansions/n64/level/LegendsPortals.h"
#include "game/level/ItemFigure.h"

namespace gdl::game {

/** Resolved engine-space collision and key-door policy. Platform readers retain
 * their own flags and animation names; no native record is forged for another game. */
struct LegendsGateDefinition {
    u32 nativeIndex = 0;
    Vec3 position{0};
    Obstacle primary; ///< Centre is relative to position, dimensions already resolved.
    std::array<Obstacle, 2> frames;
    u32 sound = 0; ///< The owning platform's token, resolved by its sound adapter.
    f32 maximumAboveFeet = std::numeric_limits<f32>::infinity();
    bool initiallyActive = false;
    bool hidden = false;
    bool rejectRetreat = false;

    LegendsGateDefinition() {
        primary.solid = false;
        for (auto& frame : frames)
            frame.solid = false;
    }
};

/** Native key-door transaction. Primary collision ends at admission, while
 * the two frame volumes remain. Opening completion only changes presentation. */
class LegendsGate {
public:
    enum class State { Closed, Opening, Open };
    explicit LegendsGate(LegendsGateDefinition definition);
    explicit LegendsGate(formats::LegendsN64Gate definition);
    const LegendsGateDefinition& definition() const { return m_definition; }
    State state() const { return m_state; }
    bool touch(const LegendsPortalVisitor& visitor, s32& keys);
    bool finishOpening();
    /** Placement flags2/4 activate on a real tick, without inventing a key debit. */
    bool activateInitially();
    void placeAt(const Vec3& feet);
    Obstacle primaryObstacle() const;
    std::array<Obstacle, 2> frameObstacles() const;

private:
    Obstacle obstacle(Obstacle collider) const;
    LegendsGateDefinition m_definition;
    State m_state = State::Closed;
    std::array<std::optional<Vec3>, 4> m_previousVisitors;
};

} // namespace gdl::game
