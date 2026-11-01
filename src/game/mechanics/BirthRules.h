#pragma once

#include "engine/core/Types.h"
#include "engine/math/Mt19937.h"

namespace gdl::game {

/** A birth into the swarm, as rules about it see it. */
struct BirthRequest {
    s32 kind = 0;
    bool frenzied = false; ///< bred past its generator's brood under a frenzy
};

/** What a body is born with beyond its kind. The defaults change nothing. */
struct BodyTraits {
    s32 affinity = 0;    ///< the element it is attuned to (`damage::kElementMask`), 0 none
    f32 healthScale = 1; ///< its health is multiplied by this
    f32 sizeScale = 1;   ///< its radius, height and reach are multiplied by this
    bool operator==(const BodyTraits&) const = default;
};

/** Rules a mechanic lays over what the swarm is born as. */
class BirthRules {
public:
    virtual ~BirthRules() = default;
    /** The traits of `birth`, drawing on the swarm's own `random` only when the rules must,
     * so that rules with nothing to say leave the swarm's stream as it was. */
    virtual BodyTraits traitsFor(const BirthRequest& birth, Mt19937& random) const = 0;
};

} // namespace gdl::game
