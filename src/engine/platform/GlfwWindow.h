#pragma once

#include "engine/platform/Window.h"

struct GLFWwindow;

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
    void waitWhileMinimized() override;
    const Input& input() const override { return m_input; }

    std::vector<const char*> requiredVulkanInstanceExtensions() const override;
    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) const override;

private:
    void pollKeyboard();
    void pollGamepads();

    GLFWwindow* m_window = nullptr;
    Input m_input;
};

} // namespace gdl
