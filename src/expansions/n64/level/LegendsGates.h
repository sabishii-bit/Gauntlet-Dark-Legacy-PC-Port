#pragma once

#include <array>

#include "expansions/n64/formats/LegendsN64Gates.h"
#include "game/level/ItemFigure.h"
#include "expansions/n64/level/LegendsPortals.h"

namespace gdl::game {

/** Native key-door transaction. Primary collision ends at admission, while
 * the two frame volumes remain. Opening completion only changes presentation. */
class LegendsGate {
public:
    enum class State { Closed, Opening, Open };
    explicit LegendsGate(formats::LegendsN64Gate definition);
    const formats::LegendsN64Gate& definition() const { return m_definition; }
    State state() const { return m_state; }
    bool touch(const LegendsPortalVisitor& visitor, s32& keys);
    bool finishOpening();
    /** Placement flags2/4 activate on a real tick, without inventing a key debit. */
    bool activateInitially();
    void placeAt(const Vec3& feet);
    Obstacle primaryObstacle() const;
    std::array<Obstacle, 2> frameObstacles() const;

private:
    Obstacle obstacle(const formats::LegendsN64GateCollider& collider) const;
    formats::LegendsN64Gate m_definition;
    State m_state = State::Closed;
};

} // namespace gdl::game
