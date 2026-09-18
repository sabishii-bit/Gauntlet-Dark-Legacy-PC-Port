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
    void setIcon(std::span<const Image> images) override;

    std::vector<const char*> requiredVulkanInstanceExtensions() const override;
    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) const override;

private:
    void pollKeyboard();
    void pollGamepads();
    static void charCallback(GLFWwindow* window, unsigned int codepoint);
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);

    GLFWwindow* m_window = nullptr;
    Input m_input;
};

} // namespace gdl
