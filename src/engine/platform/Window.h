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
    std::string title; ///< what the window's frame shows; the program names itself
    u32 width = 1280;
    u32 height = 896;
    bool resizable = true;
    WindowMode mode = WindowMode::Windowed;
    s32 monitor = -1; ///< the display to open on, in the platform's order; -1 leaves it be
    /** What the window draws through: a surface for Vulkan, or an OpenGL 3.3 context of
     * its own. Fixed once the window is made. */
    GraphicsApi api = GraphicsApi::Vulkan;
};

/** An OpenGL entry point, looked up by name through the window's context. */
using OpenGlProcedure = void (*)();

/** Operating-system window, its input state, and the glue to either graphics API: a
 * Vulkan surface, or an OpenGL context bound to one thread at a time. */
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
    /** Puts the window on the display at `index` in the platform's order: a window moves to
     * its middle, a fullscreen window keeps its mode there. -1 forgets the choice. False for
     * a display that is not attached, or where the platform has only one. */
    virtual bool selectMonitor(s32 /*index*/) { return false; }

    /** Blocks while the framebuffer is zero-sized. */
    virtual void waitWhileMinimized() = 0;

    virtual const Input& input() const = 0;

    /** Timed feedback on a physical slot. Lower priorities cannot interrupt active pulses;
     * unsupported devices are a no-op. */
    virtual bool rumble(s32 /*pad*/, u16 /*low*/, u16 /*high*/, u32 /*milliseconds*/,
                        u8 /*priority*/ = 0) {
        return false;
    }
    virtual void stopRumble() {}
    virtual std::string clipboardText() const { return {}; }
    virtual void setClipboardText(const std::string& /*text*/) {}

    /** The window's icon at one or more sizes; the system picks. Ignored where the
     * platform has no window icons. */
    virtual void setIcon(std::span<const Image> images) = 0;

    /** Client-area pointer with a pixel hotspot. Invalid images or platform failures
     * leave the current cursor unchanged. The image is copied before returning. */
    virtual bool setCursor(const Image& image, u32 hotX, u32 hotY) = 0;
    virtual void resetCursor() = 0;
    /** Hides the pointer where it is and reads its motion without bounds while `held`,
     * for a dragged look; let go, it shows again where it was. Ignored where the
     * platform has no pointer. */
    virtual void holdCursor(bool /*held*/) {}
    /** Hides the pointer where it is while the pad is in use and shows it again when the
     * mouse or keyboard is; a held pointer stays held. Ignored where the platform has no
     * pointer. */
    virtual void hideCursor(bool /*hidden*/) {}

    virtual GraphicsApi api() const = 0;

    /** Empty when Vulkan is not available through this window system. */
    virtual std::vector<const char*> requiredVulkanInstanceExtensions() const = 0;
    virtual bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) const = 0;

    /** Binds the window's OpenGL context to the calling thread, or lets it go, so another
     * thread may take it; only a window made for OpenGL has one. */
    virtual void makeContextCurrent(bool current) = 0;
    /** Shows the frame drawn into the OpenGL context's back buffer. */
    virtual void swapBuffers() = 0;
    /** How many display refreshes each OpenGL frame waits for: 0 none, 1 vertical sync. */
    virtual void setSwapInterval(s32 interval) = 0;
    /** The OpenGL entry point of that name, or null where the context lacks it. Needs the
     * context bound to the calling thread. */
    virtual OpenGlProcedure openGlProcedure(const char* name) const = 0;
};

/** Creates a GLFW-backed window. Fatal if the windowing system cannot start. */
std::unique_ptr<Window> createGlfwWindow(const WindowDesc& desc);

} // namespace gdl
