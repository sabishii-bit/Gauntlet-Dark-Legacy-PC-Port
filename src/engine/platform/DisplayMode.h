#pragma once

#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/render/RenderTypes.h"

namespace gdl {

enum class WindowMode : u8 { Windowed, Fullscreen, BorderlessFullscreen };

/** Borderless is an ordinary desktop window, not exclusive fullscreen. Wayland
 * cannot position such a window, so its compositor owns fullscreen placement. */
struct WindowPresentation {
    bool attached = false;
    bool decorated = true;
    bool autoIconify = false;
    bool operator==(const WindowPresentation&) const = default;
};

constexpr WindowPresentation windowPresentation(WindowMode mode, bool desktopPositioning = true) {
    return {mode == WindowMode::Fullscreen ||
                (mode == WindowMode::BorderlessFullscreen && !desktopPositioning),
            mode == WindowMode::Windowed, mode == WindowMode::Fullscreen};
}

/** Modes of the display hosting the window. Desktop is preserved during exclusive fullscreen. */
struct DisplayOptions {
    Extent2D desktop;
    std::vector<Extent2D> resolutions;
    Extent2D window{0, 0};             ///< current client size, including manual window resizes
    std::vector<std::string> monitors; ///< the displays attached, by name, in the platform's order
    s32 monitor = 0;                   ///< which of them hosts the window
};

} // namespace gdl
