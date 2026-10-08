#pragma once

#include <filesystem>
#include <optional>

#include "engine/render/Image.h"

namespace gdl::game {

/** Native menu marker fitted to a desktop pointer; independent of virtual HUD scaling. */
struct GameCursor {
    static constexpr u32 kSize = 40;
    Image image;
    u32 hotX = 0;
    u32 hotY = 0;

    static std::optional<GameCursor> load(const std::filesystem::path& assetRoot);
    /** Crop visible art, turn its pointed right end up-left, and antialias its reduction. */
    static std::optional<GameCursor> fromMarker(const Image& marker);
};

} // namespace gdl::game
