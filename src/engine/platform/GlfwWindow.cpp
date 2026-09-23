#include "engine/platform/GlfwWindow.h"

#include "engine/core/Types.h"

// clang-format off
// volk must precede glfw3.h so GLFW declares its Vulkan helpers.
#include <volk.h>

#include <GLFW/glfw3.h>
// clang-format on

#include <array>
#include <span>

#include "engine/core/Assert.h"
#include "engine/core/Log.h"

namespace gdl {

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
});

constexpr std::array<s32, static_cast<usize>(PadButton::Count)> kPadButtonMap{
    GLFW_GAMEPAD_BUTTON_A,           GLFW_GAMEPAD_BUTTON_B,
    GLFW_GAMEPAD_BUTTON_X,           GLFW_GAMEPAD_BUTTON_Y,
    GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER,
    GLFW_GAMEPAD_BUTTON_BACK,        GLFW_GAMEPAD_BUTTON_START,
    GLFW_GAMEPAD_BUTTON_GUIDE,       GLFW_GAMEPAD_BUTTON_LEFT_THUMB,
    GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, GLFW_GAMEPAD_BUTTON_DPAD_UP,
    GLFW_GAMEPAD_BUTTON_DPAD_RIGHT,  GLFW_GAMEPAD_BUTTON_DPAD_DOWN,
    GLFW_GAMEPAD_BUTTON_DPAD_LEFT,
};

constexpr std::array<s32, static_cast<usize>(PadAxis::Count)> kPadAxisMap{
    GLFW_GAMEPAD_AXIS_LEFT_X,  GLFW_GAMEPAD_AXIS_LEFT_Y,       GLFW_GAMEPAD_AXIS_RIGHT_X,
    GLFW_GAMEPAD_AXIS_RIGHT_Y, GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER,
};

} // namespace

GlfwWindow::GlfwWindow(const WindowDesc& desc) {
    if (glfwReferenceCount() == 0) {
        glfwSetErrorCallback(glfwErrorCallback);
        GDL_VERIFY(glfwInit() == GLFW_TRUE, "glfwInit failed");
    }
    ++glfwReferenceCount();

    GDL_VERIFY(glfwVulkanSupported() == GLFW_TRUE,
               "GLFW reports no Vulkan support (is a Vulkan-capable driver installed?)");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);

    m_window = glfwCreateWindow(static_cast<s32>(desc.width), static_cast<s32>(desc.height),
                                desc.title.c_str(), nullptr, nullptr);
    GDL_VERIFY(m_window != nullptr, "glfwCreateWindow failed");
    glfwSetWindowUserPointer(m_window, this);
    glfwSetCharCallback(m_window, &GlfwWindow::charCallback);
    glfwSetKeyCallback(m_window, &GlfwWindow::keyCallback);

    log::info("Window created: {}x{} \"{}\"", desc.width, desc.height, desc.title);
}

GlfwWindow::~GlfwWindow() {
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
    }
    if (--glfwReferenceCount() == 0) {
        glfwTerminate();
    }
}

void GlfwWindow::pollEvents() {
    m_input.beginPoll();
    glfwPollEvents();
    pollKeyboard();
    pollGamepads();
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

void GlfwWindow::pollKeyboard() {
    for (const auto& mapping : kKeyMap) {
        m_input.setKey(mapping.key, glfwGetKey(m_window, mapping.glfwKey) == GLFW_PRESS);
    }
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

void GlfwWindow::pollGamepads() {
    for (s32 pad = 0; pad < Input::kMaxPads; ++pad) {
        PadSnapshot snapshot;
        const s32 joystick = GLFW_JOYSTICK_1 + pad;
        GLFWgamepadstate state{};
        if (glfwJoystickIsGamepad(joystick) == GLFW_TRUE &&
            glfwGetGamepadState(joystick, &state) == GLFW_TRUE) {
            snapshot.connected = true;
            for (usize i = 0; i < snapshot.buttons.size(); ++i) {
                snapshot.buttons[i] = state.buttons[kPadButtonMap[i]] == GLFW_PRESS;
            }
            for (usize i = 0; i < snapshot.axes.size(); ++i) {
                f32 value = state.axes[kPadAxisMap[i]];
                if (i >= static_cast<usize>(PadAxis::LeftTrigger)) {
                    value = (value + 1.0f) * 0.5f;
                }
                snapshot.axes[i] = value;
            }
        }
        m_input.setPad(pad, snapshot);
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

void GlfwWindow::waitWhileMinimized() {
    while (framebufferSize().isZero() && !shouldClose()) {
        glfwWaitEvents();
    }
}

std::vector<const char*> GlfwWindow::requiredVulkanInstanceExtensions() const {
    u32 count = 0;
    const char** names = glfwGetRequiredInstanceExtensions(&count);
    GDL_VERIFY(names != nullptr, "glfwGetRequiredInstanceExtensions failed");
    const std::span<const char* const> extensions(names, count);
    return {extensions.begin(), extensions.end()};
}

bool GlfwWindow::createVulkanSurface(VkInstance instance, VkSurfaceKHR* outSurface) const {
    return glfwCreateWindowSurface(instance, m_window, nullptr, outSurface) == VK_SUCCESS;
}

std::unique_ptr<Window> createGlfwWindow(const WindowDesc& desc) {
    return std::make_unique<GlfwWindow>(desc);
}

} // namespace gdl
