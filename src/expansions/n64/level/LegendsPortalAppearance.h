#pragma once

#include <vector>

#include "engine/assets/AnimationSet.h"

namespace gdl::game {

/** Tower e0863d24 suppresses CENTER's mesh and XCOANI's entire branch on a
 * locked stage portal, using the decoded native OBHD part aliases. This returns
 * expanded node indices for a per-draw mask,
 * without changing the shared native tree or hiding CENTER's children. */
std::vector<usize> legendsLockedPortalMeshes(const TreeInfo& tree);

} // namespace gdl::game
