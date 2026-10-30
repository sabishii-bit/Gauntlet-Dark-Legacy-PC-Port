#include "engine/platform/GlfwWindow.h"

#include "engine/core/Types.h"

// clang-format off
// volk must precede glfw3.h so GLFW declares its Vulkan helpers.
#include <volk.h>

#include <GLFW/glfw3.h>
// clang-format on

#include <algorithm>
#include <array>
#include <limits>
#include <span>

#ifdef _WIN32
#include <SDL3/SDL_init.h>
#endif

#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/platform/DisplayTiming.h"
#include "engine/render/RenderDevice.h"

namespace gdl {
std::string GlfwWindow::clipboardText() const {
    const auto* text = glfwGetClipboardString(m_window);
    return text != nullptr ? text : "";
}
void GlfwWindow::setClipboardText(const std::string& text) {
    glfwSetClipboardString(m_window, text.c_str());
}

namespace {

s32& glfwReferenceCount() {
    static s32 count = 0;
    return count;
}

void glfwErrorCallback(s32 code, const char* description) {
    log::error("[glfw] error {}: {}", code, description);
}

struct KeyMapping {
    s32 glfwKey = 0;
    Key key = Key::Unknown;
};

constexpr auto kKeyMap = std::to_array<KeyMapping>({
    {GLFW_KEY_ESCAPE, Key::Escape},
    {GLFW_KEY_ENTER, Key::Enter},
    {GLFW_KEY_SPACE, Key::Space},
    {GLFW_KEY_TAB, Key::Tab},
    {GLFW_KEY_BACKSPACE, Key::Backspace},
    {GLFW_KEY_UP, Key::Up},
    {GLFW_KEY_DOWN, Key::Down},
    {GLFW_KEY_LEFT, Key::Left},
    {GLFW_KEY_RIGHT, Key::Right},
    {GLFW_KEY_LEFT_SHIFT, Key::LeftShift},
    {GLFW_KEY_LEFT_CONTROL, Key::LeftControl},
    {GLFW_KEY_LEFT_ALT, Key::LeftAlt},
    {GLFW_KEY_A, Key::A},
    {GLFW_KEY_B, Key::B},
    {GLFW_KEY_C, Key::C},
    {GLFW_KEY_D, Key::D},
    {GLFW_KEY_E, Key::E},
    {GLFW_KEY_F, Key::F},
    {GLFW_KEY_G, Key::G},
    {GLFW_KEY_H, Key::H},
    {GLFW_KEY_I, Key::I},
    {GLFW_KEY_J, Key::J},
    {GLFW_KEY_K, Key::K},
    {GLFW_KEY_L, Key::L},
    {GLFW_KEY_M, Key::M},
    {GLFW_KEY_N, Key::N},
    {GLFW_KEY_O, Key::O},
    {GLFW_KEY_P, Key::P},
    {GLFW_KEY_Q, Key::Q},
    {GLFW_KEY_R, Key::R},
    {GLFW_KEY_S, Key::S},
    {GLFW_KEY_T, Key::T},
    {GLFW_KEY_U, Key::U},
    {GLFW_KEY_V, Key::V},
    {GLFW_KEY_W, Key::W},
    {GLFW_KEY_X, Key::X},
    {GLFW_KEY_Y, Key::Y},
    {GLFW_KEY_Z, Key::Z},
    {GLFW_KEY_0, Key::Num0},
    {GLFW_KEY_1, Key::Num1},
    {GLFW_KEY_2, Key::Num2},
    {GLFW_KEY_3, Key::Num3},
    {GLFW_KEY_4, Key::Num4},
    {GLFW_KEY_5, Key::Num5},
    {GLFW_KEY_6, Key::Num6},
    {GLFW_KEY_7, Key::Num7},
    {GLFW_KEY_8, Key::Num8},
    {GLFW_KEY_9, Key::Num9},
    {GLFW_KEY_F1, Key::F1},
    {GLFW_KEY_F2, Key::F2},
    {GLFW_KEY_F3, Key::F3},
    {GLFW_KEY_F4, Key::F4},
    {GLFW_KEY_F5, Key::F5},
    {GLFW_KEY_F6, Key::F6},
    {GLFW_KEY_F7, Key::F7},
    {GLFW_KEY_F8, Key::F8},
    {GLFW_KEY_F9, Key::F9},
    {GLFW_KEY_F10, Key::F10},
    {GLFW_KEY_F11, Key::F11},
    {GLFW_KEY_F12, Key::F12},
    {GLFW_KEY_F13, Key::F13},
    {GLFW_KEY_F14, Key::F14},
    {GLFW_KEY_F15, Key::F15},
    {GLFW_KEY_F16, Key::F16},
    {GLFW_KEY_F17, Key::F17},
    {GLFW_KEY_F18, Key::F18},
    {GLFW_KEY_F19, Key::F19},
    {GLFW_KEY_F20, Key::F20},
    {GLFW_KEY_F21, Key::F21},
    {GLFW_KEY_F22, Key::F22},
    {GLFW_KEY_F23, Key::F23},
    {GLFW_KEY_F24, Key::F24},
    {GLFW_KEY_GRAVE_ACCENT, Key::Grave},
    {GLFW_KEY_PAGE_UP, Key::PageUp},
    {GLFW_KEY_PAGE_DOWN, Key::PageDown},
});

} // namespace

GlfwWindow::GlfwWindow(const WindowDesc& desc) : m_api(desc.api) {
    if (glfwReferenceCount() == 0) {
        glfwSetErrorCallback(glfwErrorCallback);
        GDL_VERIFY(glfwInit() == GLFW_TRUE, "glfwInit failed");
    }
    ++glfwReferenceCount();

    glfwDefaultWindowHints();
    if (m_api == GraphicsApi::OpenGl) {
        // The OpenGL device needs the 3.3 core profile: sampler objects, explicit attribute
        // locations and multisampled renderbuffers.
        glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    } else {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

    m_window = glfwCreateWindow(static_cast<s32>(desc.width), static_cast<s32>(desc.height),
                                desc.title.c_str(), nullptr, nullptr);
    if (m_window == nullptr && m_api == GraphicsApi::OpenGl) {
        // Without a window no destructor runs: let the library go here.
        if (--glfwReferenceCount() == 0) {
            glfwTerminate();
        }
        throw RenderDeviceUnavailable("No OpenGL 3.3 context could be made for the window");
    }
    GDL_VERIFY(m_window != nullptr, "glfwCreateWindow failed");
#ifdef _WIN32
    // With no SDL video thread, HID discovery runs a window-filtered PeekMessage loop
    // alongside GLFW's native pump. Windows then marks an idle game window as hung,
    // even though it still renders (SDL issue 2998). Register this as SDL's video
    // thread so GLFW alone dispatches both libraries' window messages. No SDL window
    // or renderer is created; controller discovery and hotplug remain enabled.
    GDL_VERIFY(SDL_InitSubSystem(SDL_INIT_VIDEO), "SDL window-event initialization failed");
#endif
    glfwSetWindowUserPointer(m_window, this);
    glfwSetCharCallback(m_window, &GlfwWindow::charCallback);
    glfwSetKeyCallback(m_window, &GlfwWindow::keyCallback);
    glfwSetWindowFocusCallback(m_window, [](GLFWwindow* window, s32 focused) {
        if (focused == GLFW_FALSE) {
            auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
            self->m_gamepads.stop();
        }
    });
    glfwSetMouseButtonCallback(m_window, [](GLFWwindow* window, s32 button, s32 action, s32) {
        // The five buttons GLFW numbers from the left one stand in the key table in that
        // order: left, right, middle, then the two side buttons.
        if (action == GLFW_PRESS && button >= GLFW_MOUSE_BUTTON_LEFT &&
            button <= GLFW_MOUSE_BUTTON_5) {
            auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
            self->m_input.latchKey(static_cast<Key>(static_cast<s32>(Key::MouseLeft) + button));
        }
        if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
            auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
            self->m_input.latchPointer();
        }
        if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
            auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
            self->m_input.latchPointerBack();
        }
    });
    glfwSetScrollCallback(m_window, [](GLFWwindow* window, f64, f64 vertical) {
        auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
        self->m_input.scrollPointer(static_cast<f32>(vertical));
        // A notch is a press as well, so the wheel can be bound where a button can.
        if (vertical != 0.0) {
            self->m_input.latchKey(vertical > 0.0 ? Key::ScrollUp : Key::ScrollDown);
        }
    });

    if (desc.monitor >= 0 && !selectMonitor(desc.monitor)) {
        log::warn("Saved display {} is not attached; using the one the window is on",
                  desc.monitor + 1);
    }
    if (desc.mode != WindowMode::Windowed &&
        !setDisplayMode(desc.mode, {desc.width, desc.height})) {
        log::warn("Saved display mode unavailable; retaining a window");
    }

    log::info("Window created: {}x{} \"{}\"", desc.width, desc.height, desc.title);
}

GlfwWindow::~GlfwWindow() {
    m_gamepads.stop();
    resetCursor();
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
    }
#ifdef _WIN32
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif
    if (--glfwReferenceCount() == 0) {
        glfwTerminate();
    }
}

void GlfwWindow::pollEvents() {
    m_input.beginPoll();
    glfwPollEvents();
    // Monitor removal can return GLFW to windowed mode independently of our menu.
    applyCursorMode();
    pollKeyboard();
    m_gamepads.poll(m_input, glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE);
    f64 x = 0;
    f64 y = 0;
    s32 width = 0;
    s32 height = 0;
    glfwGetCursorPos(m_window, &x, &y);
    glfwGetWindowSize(m_window, &width, &height);
    const bool inside = width > 0 && height > 0 && x >= 0 && y >= 0 && x < width && y < height &&
                        glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE;
    m_input.setPointer(
        {width > 0 ? static_cast<f32>(x / width) : 0, height > 0 ? static_cast<f32>(y / height) : 0,
         inside, inside && glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS});
}

void GlfwWindow::setIcon(std::span<const Image> images) {
    // GLFW wants writable pixel pointers, so the icons are copied for the call.
    std::vector<std::vector<u8>> pixels;
    pixels.reserve(images.size());
    std::vector<GLFWimage> handles;
    for (const Image& image : images) {
        pixels.emplace_back(image.pixels);
        handles.push_back(GLFWimage{static_cast<s32>(image.width), static_cast<s32>(image.height),
                                    pixels.back().data()});
    }
    glfwSetWindowIcon(m_window, static_cast<s32>(handles.size()), handles.data());
}

bool GlfwWindow::setCursor(const Image& image, u32 hotX, u32 hotY) {
    if (image.width == 0 || image.height == 0 ||
        image.width > static_cast<u32>(std::numeric_limits<s32>::max()) ||
        image.height > static_cast<u32>(std::numeric_limits<s32>::max()) ||
        image.pixels.size() != u64{image.width} * image.height * 4 || hotX >= image.width ||
        hotY >= image.height) {
        return false;
    }
    // GLFW accepts straight RGBA and copies the pixels during creation.
    auto pixels = image.pixels;
    const GLFWimage bitmap{static_cast<s32>(image.width), static_cast<s32>(image.height),
                           pixels.data()};
    GLFWcursor* cursor = glfwCreateCursor(&bitmap, static_cast<s32>(hotX), static_cast<s32>(hotY));
    if (cursor == nullptr) {
        return false;
    }
    glfwSetCursor(m_window, cursor);
    glfwDestroyCursor(m_cursor);
    m_cursor = cursor;
    return true;
}

void GlfwWindow::resetCursor() {
    if (m_cursor != nullptr) {
        glfwSetCursor(m_window, nullptr);
        glfwDestroyCursor(m_cursor);
        m_cursor = nullptr;
    }
}

void GlfwWindow::pollKeyboard() {
    for (const auto& mapping : kKeyMap) {
        m_input.setKey(mapping.key, glfwGetKey(m_window, mapping.glfwKey) == GLFW_PRESS);
    }
    for (s32 button = GLFW_MOUSE_BUTTON_LEFT; button <= GLFW_MOUSE_BUTTON_5; ++button) {
        m_input.setKey(static_cast<Key>(static_cast<s32>(Key::MouseLeft) + button),
                       glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE &&
                           glfwGetMouseButton(m_window, button) == GLFW_PRESS);
    }
    // The wheel is never held: its notches are the latched presses alone.
    m_input.setKey(Key::ScrollUp, false);
    m_input.setKey(Key::ScrollDown, false);
}

void GlfwWindow::charCallback(GLFWwindow* window, u32 codepoint) {
    if (auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window))) {
        self->m_input.addTypedChar(codepoint);
    }
}

/** A press is latched as it happens, so a tap over between polls still reaches the game. */
void GlfwWindow::keyCallback(GLFWwindow* window, s32 key, s32 /*scancode*/, s32 action,
                             s32 /*mods*/) {
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self == nullptr || action != GLFW_PRESS) {
        return;
    }
    for (const auto& mapping : kKeyMap) {
        if (mapping.glfwKey == key) {
            self->m_input.latchKey(mapping.key);
            return;
        }
    }
}

bool GlfwWindow::shouldClose() const {
    return glfwWindowShouldClose(m_window) == GLFW_TRUE;
}

void GlfwWindow::requestClose() {
    glfwSetWindowShouldClose(m_window, GLFW_TRUE);
}

Extent2D GlfwWindow::framebufferSize() const {
    s32 width = 0;
    s32 height = 0;
    glfwGetFramebufferSize(m_window, &width, &height);
    return Extent2D{static_cast<u32>(std::max(width, 0)), static_cast<u32>(std::max(height, 0))};
}

GLFWmonitor* GlfwWindow::activeMonitor() const {
    if (auto* monitor = glfwGetWindowMonitor(m_window)) {
        return monitor;
    }
    auto* selected = glfwGetPrimaryMonitor();
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        return selected;
    }
    s32 x = 0;
    s32 y = 0;
    s32 width = 0;
    s32 height = 0;
    glfwGetWindowPos(m_window, &x, &y);
    glfwGetWindowSize(m_window, &width, &height);
    s32 count = 0;
    auto** monitors = glfwGetMonitors(&count);
    s64 bestArea = 0;
    for (auto* monitor : std::span(monitors, static_cast<usize>(count))) {
        const auto* mode = glfwGetVideoMode(monitor);
        if (mode == nullptr) {
            continue;
        }
        s32 mx = 0;
        s32 my = 0;
        glfwGetMonitorPos(monitor, &mx, &my);
        const auto overlapWidth =
            std::max(0, std::min(x + width, mx + mode->width) - std::max(x, mx));
        const auto overlapHeight =
            std::max(0, std::min(y + height, my + mode->height) - std::max(y, my));
        const auto area = static_cast<s64>(overlapWidth) * overlapHeight;
        if (area > bestArea) {
            bestArea = area;
            selected = monitor;
        }
    }
    return selected;
}

GLFWmonitor* GlfwWindow::targetMonitor() const {
    s32 count = 0;
    auto** monitors = glfwGetMonitors(&count);
    if (monitors != nullptr && m_preferredMonitor >= 0 && m_preferredMonitor < count) {
        return std::span(monitors,
                         static_cast<usize>(count))[static_cast<usize>(m_preferredMonitor)];
    }
    return activeMonitor();
}

bool GlfwWindow::selectMonitor(s32 index) {
    s32 count = 0;
    auto** monitors = glfwGetMonitors(&count);
    if (index >= count) {
        return false;
    }
    m_preferredMonitor = std::max(index, -1);
    if (index < 0 || monitors == nullptr) {
        return true;
    }
    auto* monitor = std::span(monitors, static_cast<usize>(count))[static_cast<usize>(index)];
    if (auto* attached = glfwGetWindowMonitor(m_window)) {
        if (attached == monitor) {
            return true;
        }
        // The same mode on the display chosen, at its own desktop size when the current
        // size is not among its modes.
        const WindowMode mode = windowMode();
        s32 width = 0;
        s32 height = 0;
        glfwGetWindowSize(m_window, &width, &height);
        const Extent2D size{static_cast<u32>(std::max(0, width)),
                            static_cast<u32>(std::max(0, height))};
        return setDisplayMode(mode, size) || setDisplayMode(mode, displayOptions().desktop);
    }
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        // A window moves to the middle of the display's work area.
        s32 x = 0;
        s32 y = 0;
        s32 width = 0;
        s32 height = 0;
        glfwGetMonitorWorkarea(monitor, &x, &y, &width, &height);
        s32 windowWidth = 0;
        s32 windowHeight = 0;
        glfwGetWindowSize(m_window, &windowWidth, &windowHeight);
        glfwSetWindowPos(m_window, x + (width - windowWidth) / 2, y + (height - windowHeight) / 2);
    }
    return true;
}

DisplayOptions GlfwWindow::displayOptions() const {
    DisplayOptions result;
    s32 width = 0;
    s32 height = 0;
    glfwGetWindowSize(m_window, &width, &height);
    result.window = {static_cast<u32>(std::max(0, width)), static_cast<u32>(std::max(0, height))};
    auto* monitor = targetMonitor();
    s32 monitorCount = 0;
    auto** monitors = glfwGetMonitors(&monitorCount);
    if (monitors != nullptr) {
        for (auto* attached : std::span(monitors, static_cast<usize>(std::max(0, monitorCount)))) {
            const char* name = glfwGetMonitorName(attached);
            if (attached == monitor) {
                result.monitor = static_cast<s32>(result.monitors.size());
            }
            result.monitors.emplace_back(name != nullptr ? name : "");
        }
    }
    if (monitor == nullptr) {
        return result;
    }
    if (const auto* mode = glfwGetVideoMode(monitor)) {
        result.desktop = {static_cast<u32>(mode->width), static_cast<u32>(mode->height)};
    }
    if (glfwGetWindowMonitor(m_window) == m_desktopMonitor && monitor == m_desktopMonitor) {
        result.desktop = m_desktopSize;
    }
    s32 count = 0;
    const auto* modes = glfwGetVideoModes(monitor, &count);
    const auto* current = glfwGetVideoMode(monitor);
    auto refresh = current != nullptr ? current->refreshRate : 60;
    if (glfwGetWindowMonitor(m_window) == m_desktopMonitor && monitor == m_desktopMonitor) {
        refresh = m_desktopRefresh;
    }
    for (const auto& mode : std::span(modes, static_cast<usize>(count))) {
        if (mode.refreshRate > refresh) {
            continue;
        }
        const Extent2D size{static_cast<u32>(mode.width), static_cast<u32>(mode.height)};
        if (std::ranges::find(result.resolutions, size) == result.resolutions.end()) {
            result.resolutions.push_back(size);
        }
    }
    std::ranges::sort(result.resolutions, [](Extent2D a, Extent2D b) {
        return a.width == b.width ? a.height < b.height : a.width < b.width;
    });
    return result;
}

WindowMode GlfwWindow::windowMode() const {
    if (glfwGetWindowMonitor(m_window) == nullptr) {
        return WindowMode::Windowed;
    }
    return glfwGetWindowAttrib(m_window, GLFW_AUTO_ICONIFY) == GLFW_TRUE
               ? WindowMode::Fullscreen
               : WindowMode::BorderlessFullscreen;
}

void GlfwWindow::holdCursor(bool held) {
    m_cursorHeld = held;
    applyCursorMode();
}

void GlfwWindow::hideCursor(bool hidden) {
    m_cursorHidden = hidden;
    applyCursorMode();
}

void GlfwWindow::applyCursorMode() {
    // Disabled, GLFW hides the pointer, keeps its place to put it back and reports its
    // motion without the screen's edges in the way. Hidden only hides it; it cannot be
    // confined at the same time, so a pad-driven fullscreen game lets it go until the
    // mouse moves again. Captured remains visible; GLFW releases the OS confinement on
    // focus loss.
    s32 mode = GLFW_CURSOR_NORMAL;
    if (m_cursorHeld) {
        mode = GLFW_CURSOR_DISABLED;
    } else if (m_cursorHidden) {
        mode = GLFW_CURSOR_HIDDEN;
    } else if (windowMode() == WindowMode::Fullscreen) {
        mode = GLFW_CURSOR_CAPTURED;
    }
    if (glfwGetInputMode(m_window, GLFW_CURSOR) != mode) {
        glfwSetInputMode(m_window, GLFW_CURSOR, mode);
    }
}

bool GlfwWindow::cursorCaptured() const {
    return glfwGetInputMode(m_window, GLFW_CURSOR) == GLFW_CURSOR_CAPTURED;
}

bool GlfwWindow::setDisplayMode(WindowMode mode, Extent2D resolution) {
    // GLFW takes signed dimensions; reject invalid persisted values before conversion.
    if (resolution.isZero() || resolution.width > 32768 || resolution.height > 32768) {
        return false;
    }
    auto* monitor = targetMonitor();
    const auto options = displayOptions();
    if (mode != WindowMode::Windowed && (monitor == nullptr || options.desktop.isZero())) {
        return false;
    }
    if (mode == WindowMode::Fullscreen &&
        std::ranges::find(options.resolutions, resolution) == options.resolutions.end()) {
        return false;
    }
    const bool attached = glfwGetWindowMonitor(m_window) != nullptr;
    if (!attached && glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) {
        glfwGetWindowPos(m_window, &m_windowX, &m_windowY);
    }
    // The desktop of the display taken is kept to give back; a move to another display
    // records that one's.
    if ((!attached || monitor != m_desktopMonitor) && monitor != nullptr) {
        m_desktopMonitor = monitor;
        m_desktopSize = options.desktop;
        if (const auto* desktop = glfwGetVideoMode(monitor)) {
            m_desktopRefresh = desktop->refreshRate;
        }
    }
    if (mode == WindowMode::BorderlessFullscreen) {
        resolution = options.desktop;
    }
    s32 refresh = m_desktopRefresh;
    if (mode == WindowMode::Fullscreen) {
        s32 count = 0;
        const auto* modes = glfwGetVideoModes(monitor, &count);
        refresh = 0;
        for (const auto& candidate : std::span(modes, static_cast<usize>(count))) {
            if (candidate.width == static_cast<s32>(resolution.width) &&
                candidate.height == static_cast<s32>(resolution.height) &&
                candidate.refreshRate <= m_desktopRefresh) {
                refresh = std::max(refresh, candidate.refreshRate);
            }
        }
        if (refresh == 0) {
            return false;
        }
    }
    auto* previousMonitor = glfwGetWindowMonitor(m_window);
    s32 previousWidth = 0;
    s32 previousHeight = 0;
    glfwGetWindowSize(m_window, &previousWidth, &previousHeight);
    const auto* previousMode =
        previousMonitor != nullptr ? glfwGetVideoMode(previousMonitor) : nullptr;
    const s32 previousRefresh =
        previousMode != nullptr ? previousMode->refreshRate : GLFW_DONT_CARE;
    const s32 previousIconify = glfwGetWindowAttrib(m_window, GLFW_AUTO_ICONIFY);
    const s32 previousCursor = glfwGetInputMode(m_window, GLFW_CURSOR);
    glfwGetError(nullptr); // Discard earlier unrelated input/monitor errors.
    glfwSetWindowAttrib(m_window, GLFW_AUTO_ICONIFY,
                        mode == WindowMode::Fullscreen ? GLFW_TRUE : GLFW_FALSE);
    // A monitor-attached window at the desktop mode is GLFW's borderless fullscreen.
    // Never select a higher refresh rate just because a resolution offers one.
    glfwSetWindowMonitor(m_window, mode == WindowMode::Windowed ? nullptr : monitor, m_windowX,
                         m_windowY, static_cast<s32>(resolution.width),
                         static_cast<s32>(resolution.height), refresh);
    applyCursorMode();
    if (glfwGetError(nullptr) == GLFW_NO_ERROR) {
        return true;
    }
    // Restore the actual previous window (including a manually resized client area).
    glfwSetWindowAttrib(m_window, GLFW_AUTO_ICONIFY, previousIconify);
    glfwSetInputMode(m_window, GLFW_CURSOR, previousCursor);
    glfwSetWindowMonitor(m_window, previousMonitor, m_windowX, m_windowY, previousWidth,
                         previousHeight, previousRefresh);
    return false;
}

u32 GlfwWindow::refreshRate() const {
    auto* activeMonitor = glfwGetWindowMonitor(m_window);
    // Wayland does not expose window desktop coordinates. Use its primary display
    // rather than issuing an unsupported position query on every rendered frame.
    if (activeMonitor == nullptr && glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        activeMonitor = glfwGetPrimaryMonitor();
        if (activeMonitor == nullptr) {
            return DisplayRefresh::kFallbackRate;
        }
    }
    if (activeMonitor != nullptr) {
        const auto* mode = glfwGetVideoMode(activeMonitor);
        return mode != nullptr && mode->refreshRate > 0 ? static_cast<u32>(mode->refreshRate)
                                                        : DisplayRefresh::kFallbackRate;
    }
    s32 x = 0;
    s32 y = 0;
    s32 width = 0;
    s32 height = 0;
    glfwGetWindowPos(m_window, &x, &y);
    glfwGetWindowSize(m_window, &width, &height);
    DisplayRefresh selected(
        {x, y, static_cast<u32>(std::max(width, 0)), static_cast<u32>(std::max(height, 0))});
    s32 count = 0;
    auto** monitors = glfwGetMonitors(&count);
    if (monitors == nullptr || count <= 0) {
        return selected.rate();
    }
    for (auto* monitor : std::span(monitors, static_cast<usize>(count))) {
        const auto* mode = glfwGetVideoMode(monitor);
        if (mode == nullptr) {
            continue;
        }
        glfwGetMonitorPos(monitor, &x, &y);
        selected.consider({x, y, static_cast<u32>(std::max(mode->width, 0)),
                           static_cast<u32>(std::max(mode->height, 0))},
                          static_cast<u32>(std::max(mode->refreshRate, 0)));
    }
    return selected.rate();
}

void GlfwWindow::waitWhileMinimized() {
    while (framebufferSize().isZero() && !shouldClose()) {
        glfwWaitEvents();
    }
}

std::vector<const char*> GlfwWindow::requiredVulkanInstanceExtensions() const {
    if (glfwVulkanSupported() != GLFW_TRUE) {
        return {};
    }
    u32 count = 0;
    const char** names = glfwGetRequiredInstanceExtensions(&count);
    if (names == nullptr) {
        return {};
    }
    const std::span<const char* const> extensions(names, count);
    return {extensions.begin(), extensions.end()};
}

bool GlfwWindow::createVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) const {
    return glfwCreateWindowSurface(instance, m_window, nullptr, outSurface) == VK_SUCCESS;
}

void GlfwWindow::makeContextCurrent(bool current) {
    glfwMakeContextCurrent(current ? m_window : nullptr);
}

void GlfwWindow::swapBuffers() {
    glfwSwapBuffers(m_window);
}

void GlfwWindow::setSwapInterval(s32 interval) {
    glfwSwapInterval(interval);
}

OpenGlProcedure GlfwWindow::openGlProcedure(const char* name) const {
    return glfwGetProcAddress(name);
}

std::unique_ptr<Window> createGlfwWindow(const WindowDesc& desc) {
    return std::make_unique<GlfwWindow>(desc);
}

} // namespace gdl
