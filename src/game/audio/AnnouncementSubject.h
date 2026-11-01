#pragma once

#include "engine/core/Types.h"

namespace gdl::game {

/** Actual player identity for an optional complete narrator replacement.
 * A bank's name clip is not sufficient: costumes can share those names. */
struct AnnouncementSubject {
    s32 character = -1;
    s32 color = -1;
    bool pojo = false;
};

} // namespace gdl::game
