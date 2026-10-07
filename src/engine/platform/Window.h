#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/platform/DisplayMode.h"
#include "engine/platform/Input.h"
#include "engine/render/Image.h"
#include "engine/render/RenderTypes.h"
#include "engine/render/VulkanHandles.h"

namespace gdl {

struct WindowDesc {
    std::string title = "Gauntlet Dark Legacy";
    u32 width = 1280;
    u32 height = 896;
    bool resizable = true;
    WindowMode mode = WindowMode::Windowed;
};

/** Operating-system window, its input state, and the Vulkan surface glue. */
class Window {
public:
    Window() = default;
    virtual ~Window() = default;

    GDL_NON_COPYABLE_NON_MOVABLE(Window);

    virtual void pollEvents() = 0;
    virtual bool shouldClose() const = 0;
    virtual void requestClose() = 0;

    /** Drawable size in pixels. */
    virtual Extent2D framebufferSize() const = 0;

    /** Current display refresh rate, following the window across monitors; 60 if unavailable. */
    virtual u32 refreshRate() const = 0;

    virtual DisplayOptions displayOptions() const = 0;
    virtual WindowMode windowMode() const = 0;
    virtual bool cursorCaptured() const = 0;
    /** Apply a supported display mode without recreating the rendering surface. */
    virtual bool setDisplayMode(WindowMode mode, Extent2D resolution) = 0;

    /** Blocks while the framebuffer is zero-sized. */
    virtual void waitWhileMinimized() = 0;

    virtual const Input& input() const = 0;

    /** Timed motor feedback on a physical input slot; unsupported devices are a no-op. */
    virtual bool rumble(s32 /*pad*/, u16 /*low*/, u16 /*high*/, u32 /*milliseconds*/) {
        return false;
    }
    virtual void stopRumble() {}

    /** The window's icon at one or more sizes; the system picks. Ignored where the
     * platform has no window icons. */
    virtual void setIcon(std::span<const Image> images) = 0;

    virtual std::vector<const char*> requiredVulkanInstanceExtensions() const = 0;
    virtual bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) const = 0;
};

/** Creates a GLFW-backed window. Fatal if the windowing system cannot start. */
std::unique_ptr<Window> createGlfwWindow(const WindowDesc& desc);

} // namespace gdl
