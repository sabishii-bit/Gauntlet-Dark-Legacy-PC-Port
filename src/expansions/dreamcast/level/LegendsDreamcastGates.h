#pragma once

#include <optional>
#include <string>

#include "expansions/n64/level/LegendsGates.h"
#include "formats/WorldFile.h"

namespace gdl::game {

/** Resolves ordinary Dreamcast key doors into engine-space collision. Triangle
 * doors driven by level scripts are diagnosed, never approximated by a radius. */
std::optional<LegendsGateDefinition>
legendsDreamcastGate(const formats::ItemInfoRecord& info,
                     const formats::ItemInstanceRecord& instance, u32 nativeIndex,
                     std::string* diagnostic = nullptr);

} // namespace gdl::game
