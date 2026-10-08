#pragma once

#include "engine/core/Types.h"
#include "engine/platform/Gamepads.h"
#include "engine/platform/Window.h"

struct GLFWwindow;
struct GLFWmonitor;
struct GLFWcursor;

namespace gdl {

class GlfwWindow final : public Window {
public:
    explicit GlfwWindow(const WindowDesc& desc);
    ~GlfwWindow() override;

    GDL_NON_COPYABLE_NON_MOVABLE(GlfwWindow);

    void pollEvents() override;
    bool shouldClose() const override;
    void requestClose() override;
    Extent2D framebufferSize() const override;
    u32 refreshRate() const override;
    DisplayOptions displayOptions() const override;
    WindowMode windowMode() const override;
    bool cursorCaptured() const override;
    bool setDisplayMode(WindowMode mode, Extent2D resolution) override;
    void waitWhileMinimized() override;
    const Input& input() const override { return m_input; }
    bool rumble(s32 pad, u16 low, u16 high, u32 milliseconds, u8 priority = 0) override {
        return m_gamepads.rumble(pad, low, high, milliseconds, priority);
    }
    void stopRumble() override { m_gamepads.stop(); }
    void setIcon(std::span<const Image> images) override;
    bool setCursor(const Image& image, u32 hotX, u32 hotY) override;
    void resetCursor() override;

    std::vector<const char*> requiredVulkanInstanceExtensions() const override;
    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) const override;

private:
    void pollKeyboard();
    GLFWmonitor* activeMonitor() const;
    static void charCallback(GLFWwindow* window, u32 codepoint);
    static void keyCallback(GLFWwindow* window, s32 key, s32 scancode, s32 action, s32 mods);

    GLFWwindow* m_window = nullptr;
    GLFWcursor* m_cursor = nullptr;
    Input m_input;
    Gamepads m_gamepads;
    GLFWmonitor* m_desktopMonitor = nullptr;
    Extent2D m_desktopSize;
    s32 m_desktopRefresh = 60;
    s32 m_windowX = 100;
    s32 m_windowY = 100;
};

} // namespace gdl
