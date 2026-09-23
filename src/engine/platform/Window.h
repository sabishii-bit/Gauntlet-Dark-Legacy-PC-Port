#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/core/SpecialMembers.h"
#include "engine/platform/Input.h"
#include "engine/render/Image.h"
#include "engine/render/RenderTypes.h"
#include "engine/render/VulkanHandles.h"

namespace gdl {

struct WindowDesc {
    std::string title = "Gauntlet Dark Legacy";
    unsigned int width = 1280;
    unsigned int height = 896;
    bool resizable = true;
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

    /** Blocks while the framebuffer is zero-sized. */
    virtual void waitWhileMinimized() = 0;

    virtual const Input& input() const = 0;

    /** The window's icon at one or more sizes; the system picks. Ignored where the
     * platform has no window icons. */
    virtual void setIcon(std::span<const Image> images) = 0;

    virtual std::vector<const char*> requiredVulkanInstanceExtensions() const = 0;
    virtual bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) const = 0;
};

/** Creates a GLFW-backed window. Fatal if the windowing system cannot start. */
std::unique_ptr<Window> createGlfwWindow(const WindowDesc& desc);

} // namespace gdl
