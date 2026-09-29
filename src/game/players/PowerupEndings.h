#pragma once

#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

/**
 * The sounds a character makes as its specials wear off or are switched off, from the flags
 * it wore last frame against this one's (PlayerProcessPowerups' old_flags): levitation ending
 * (fn_8009D4F0), growth ending (fn_8009D560, only as the body goes back to its plain size: an
 * ogre and a level 99 never do) and Pojo leaving (fn_8009D5A0).
 */
struct PowerupEndings {
    static constexpr std::string_view kLevitationDown = "S_LEVITATEDOWN";
    static constexpr std::string_view kUngrow = "S_UNGROW";
    static constexpr std::string_view kUnpojo = "S_UNPOJO";

    /** What sounds as the worn specials go from `before` to `after`; `plainSize` is whether
     * the body is now at its plain size. */
    static std::vector<std::string_view> soundsOf(u32 before, u32 after, bool plainSize);
};

} // namespace gdl::game
