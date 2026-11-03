#pragma once

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {
struct CharacterSave;

/** What the level itself does as its boss wakes and a legend weapon is spent. */
class EncounterLevel {
public:
    virtual ~EncounterLevel() = default;
    /** The boss woke this tick, at `at`. */
    virtual void awoke(const Vec3& at) = 0;
    /** A legend weapon brandished by `bearer` in `realm` is spent: on an imported level its
     * own legends, never the relic. */
    virtual void spendLegend(CharacterSave& bearer, s32 realm) = 0;
};
} // namespace gdl::game
