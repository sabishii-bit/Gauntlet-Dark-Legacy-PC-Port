#pragma once

#include <filesystem>
#include <memory>

#include "engine/assets/ItemArchive.h"

namespace gdl::game {

/** Decode one native Dreamcast library into the engine's asset interface.
 * A missing optional library returns null; malformed existing data is rejected. */
std::unique_ptr<ItemArchive> loadLegendsDreamcastArchive(const std::filesystem::path& directory);

} // namespace gdl::game
