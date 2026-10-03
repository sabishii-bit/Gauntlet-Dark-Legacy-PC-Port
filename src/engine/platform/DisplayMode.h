#pragma once

#include <vector>

#include "engine/render/RenderTypes.h"

namespace gdl {

enum class WindowMode : u8 { Windowed, Fullscreen, BorderlessFullscreen };

/** Modes of the display hosting the window. Desktop is preserved during exclusive fullscreen. */
struct DisplayOptions {
    Extent2D desktop;
    std::vector<Extent2D> resolutions;
};

} // namespace gdl
