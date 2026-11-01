#pragma once

#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** One of the swarm gone from the level, as rules about what it leaves see it. */
struct BodyFall {
    s32 kind = 0;
    s32 affinity = 0; ///< the element it was attuned to, 0 none (`BodyTraits::affinity`)
    s32 player = -1;  ///< who felled it, -1 for nobody
    Vec3 position{0.0f};
    bool killed = false; ///< slain, rather than retired or recycled
};

/** Where rules leave what a fallen body drops: thrown in a bag that opens where it lands,
 * as the great ones' carried pickups are (StartBagFX). */
class BagDrops {
public:
    virtual ~BagDrops() = default;
    /** Throws a bag from `at` holding the level item named `item`. */
    virtual void leave(std::string_view item, const Vec3& at) = 0;
};

/** Rules a mechanic lays over what the swarm leaves behind. */
class FallRules {
public:
    virtual ~FallRules() = default;
    virtual void fell(const BodyFall& fall, BagDrops& drops) = 0;
};

} // namespace gdl::game
