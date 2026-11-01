#pragma once

#include <optional>

#include "engine/core/Types.h"

namespace gdl::game {

/** Rules about a run of close blows a mechanic may lay over the original's. */
class RunRules {
public:
    virtual ~RunRules() = default;
    /** The quiet a run of `count` blows survives, in seconds; nothing to say keeps the
     * original's (`MeleeStreak::kOriginalQuiet`). */
    virtual std::optional<f32> quietWindow(s32 count) const = 0;
};

} // namespace gdl::game
