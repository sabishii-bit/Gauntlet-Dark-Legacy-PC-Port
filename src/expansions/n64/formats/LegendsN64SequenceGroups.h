#pragma once
#include <span>
#include <string_view>

#include "formats/AnimationTree.h"

namespace gdl::formats {
/** Reassembles decoded GROUP:FOURCC trees into one stable hierarchy per group.
 * Native combatants address sequences within the same BODY, unlike item figures.
 * Rejects structurally different sequence hierarchies instead of borrowing poses. */
void combineLegendsN64SequenceGroups(AnimationFile& animation,
                                     std::span<const std::string_view> selected = {});
} // namespace gdl::formats
