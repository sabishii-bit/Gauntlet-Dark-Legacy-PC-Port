#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/ItemArchive.h"

#include "game/bosses/LegendProfile.h"

namespace gdl::game {
struct LegendsLegendBinding {
    std::unique_ptr<ItemArchive> archive;
    LegendProfile profile;
};
/** Native weapon art, carried/thrown by the common GDL rite. The original's
 * dedicated camera cinematic and code-generated particle emitters are not
 * replaced with unrelated GDL assets. Index4 has no stage-eight rite. */
LegendsLegendBinding loadLegendsLegend(const std::filesystem::path& nativeRoot, s32 index,
                                       std::span<const u8> program,
                                       std::vector<std::string>& warnings);
/** Native one-shot calls at the equivalent reveal, launch and impact moments. */
std::optional<u32> legendsLegendSound(s32 index, LegendSound phase);
} // namespace gdl::game
