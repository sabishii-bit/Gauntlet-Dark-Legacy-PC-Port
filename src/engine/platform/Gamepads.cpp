#include "engine/platform/Gamepads.h"

#include <algorithm>
#include <format>
#include <span>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl {
namespace {
constexpr std::array kButtons{SDL_GAMEPAD_BUTTON_SOUTH,         SDL_GAMEPAD_BUTTON_EAST,
                              SDL_GAMEPAD_BUTTON_WEST,          SDL_GAMEPAD_BUTTON_NORTH,
                              SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
                              SDL_GAMEPAD_BUTTON_BACK,          SDL_GAMEPAD_BUTTON_START,
                              SDL_GAMEPAD_BUTTON_GUIDE,         SDL_GAMEPAD_BUTTON_LEFT_STICK,
                              SDL_GAMEPAD_BUTTON_RIGHT_STICK,   SDL_GAMEPAD_BUTTON_DPAD_UP,
                              SDL_GAMEPAD_BUTTON_DPAD_RIGHT,    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                              SDL_GAMEPAD_BUTTON_DPAD_LEFT};
constexpr std::array kAxes{SDL_GAMEPAD_AXIS_LEFTX,        SDL_GAMEPAD_AXIS_LEFTY,
                           SDL_GAMEPAD_AXIS_RIGHTX,       SDL_GAMEPAD_AXIS_RIGHTY,
                           SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};

std::string text(const char* value) {
    return value != nullptr ? value : "";
}
f32 axisValue(s16 value) {
    return static_cast<f32>(value) / (value < 0 ? 32768.0f : 32767.0f);
}
std::string deviceGuid(SDL_Joystick* joystick) {
    const auto guid = SDL_GetJoystickGUID(joystick);
    const auto bytes = std::to_array(guid.data);
#ifdef _WIN32
    constexpr bool kWindows = true;
#else
    constexpr bool kWindows = false;
#endif
    return Gamepads::configurationGuid(bytes, text(SDL_GetJoystickName(joystick)), kWindows);
}
} // namespace

std::string Gamepads::configurationGuid(const std::array<u8, 16>& guid, std::string_view name,
                                        bool windows) {
    if (windows && guid[14] == 'x') {
        return std::format("78696e707574{:02x}000000000000000000", guid[15]);
    }
    auto bytes = guid;
    bytes[2] = bytes[3] = bytes[14] = bytes[15] = 0;
    const bool hasIds = (bytes[4] != 0 || bytes[5] != 0) && (bytes[8] != 0 || bytes[9] != 0);
    if (windows) {
        bytes[0] = hasIds ? 3 : 5;
        bytes[1] = bytes[12] = bytes[13] = 0;
    }
    if (!hasIds || (!windows && bytes[12] == 0 && bytes[13] == 0)) {
        std::fill(bytes.begin() + 4, bytes.end(), 0);
        for (usize i = 0; i < std::min(name.size(), usize{11}); ++i) {
            bytes[i + 4] = static_cast<u8>(name[i]);
        }
    }
    std::string result;
    for (const u8 byte : bytes) {
        result += std::format("{:02x}", byte);
    }
    return result;
}

namespace {
bool initializeGamepads() {
    // GLFW owns the window and focus. SDL handles controllers only; don't require an
    // SDL video window to read them. Our focus gate suppresses feedback in the background.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef _WIN32
    // Retain XInput's GUID/subtype and axis/button layout for existing Windows profiles.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
#endif
    return SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}
} // namespace

Gamepads::Gamepads() : m_ready(initializeGamepads()) {
    if (!m_ready) {
        log::warn("Controller initialization failed: {}", SDL_GetError());
        return;
    }
    SDL_SetJoystickEventsEnabled(false);
    SDL_SetGamepadEventsEnabled(false);
}

Gamepads::~Gamepads() {
    for (auto& device : m_devices) {
        close(device);
    }
    if (m_ready) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
}

void Gamepads::close(Device& device) {
    if (device.joystick != nullptr) {
        SDL_RumbleJoystick(device.joystick, 0, 0, 0);
        if (device.gamepad != nullptr) {
            SDL_CloseGamepad(device.gamepad);
        } else {
            SDL_CloseJoystick(device.joystick);
        }
    }
    device.joystick = nullptr;
    device.gamepad = nullptr;
    device.id = 0;
    device.rumbleUntil = 0;
    device.rumblePriority = 0;
}

void Gamepads::discover() {
    s32 count = 0;
    auto* ids = SDL_GetJoysticks(&count);
    const std::span connected(ids, static_cast<usize>(count));
    for (const auto id : connected) {
        if (std::ranges::any_of(m_devices, [id](const Device& d) { return d.id == id; })) {
            continue;
        }
        Device device;
        device.id = id;
        if (SDL_IsGamepad(id)) {
            device.gamepad = SDL_OpenGamepad(id);
            if (device.gamepad != nullptr) {
                device.joystick = SDL_GetGamepadJoystick(device.gamepad);
            }
        } else {
            device.joystick = SDL_OpenJoystick(id);
        }
        if (device.joystick == nullptr) {
            continue;
        }
        device.guid = deviceGuid(device.joystick);
        device.path = text(SDL_GetJoystickPath(device.joystick));
        // std::array has pointer iterators on libstdc++, checked objects on MSVC.
        // NOLINTNEXTLINE(readability-qualified-auto): keep both iterator types.
        auto slot = std::ranges::find_if(m_devices, [&](const Device& old) {
            return old.id == 0 && old.guid == device.guid && old.path == device.path;
        });
        if (slot == m_devices.end()) {
            slot = std::ranges::find_if(
                m_devices, [](const Device& old) { return old.id == 0 && old.guid.empty(); });
        }
        if (slot == m_devices.end()) {
            // An unplugged twin still reserves its occurrence in saved profiles. Reusing
            // that slot for another model would silently renumber the twin still in play.
            slot = std::ranges::find_if(m_devices, [&](const Device& old) {
                return old.id == 0 && std::ranges::none_of(m_devices, [&](const Device& live) {
                           return live.id != 0 && live.guid == old.guid;
                       });
            });
        }
        if (slot != m_devices.end()) {
            *slot = std::move(device);
        } else {
            close(device);
        }
    }
    SDL_free(ids);
}

PadSnapshot Gamepads::snapshot(const Device& device) {
    PadSnapshot result;
    if (device.joystick == nullptr) {
        return result;
    }
    result.connected = true;
    result.guid = device.guid;
    result.name = text(device.gamepad != nullptr ? SDL_GetGamepadName(device.gamepad)
                                                 : SDL_GetJoystickName(device.joystick));
    result.rumbleSupported = SDL_GetBooleanProperty(SDL_GetJoystickProperties(device.joystick),
                                                    SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
    if (device.gamepad != nullptr) {
        for (usize i = 0; i < kButtons.size(); ++i) {
            result.buttons[i] = SDL_GetGamepadButton(device.gamepad, kButtons[i]);
        }
        for (usize i = 0; i < kAxes.size(); ++i) {
            result.axes[i] = axisValue(SDL_GetGamepadAxis(device.gamepad, kAxes[i]));
        }
    } else {
        const s32 buttons = std::min(SDL_GetNumJoystickButtons(device.joystick), 32);
        for (s32 i = 0; i < buttons; ++i) {
            result.buttons[static_cast<usize>(PadButton::Button1) + static_cast<usize>(i)] =
                SDL_GetJoystickButton(device.joystick, i);
        }
        if (SDL_GetNumJoystickAxes(device.joystick) >= 2) {
            result.axes[0] = axisValue(SDL_GetJoystickAxis(device.joystick, 0));
            result.axes[1] = axisValue(SDL_GetJoystickAxis(device.joystick, 1));
        }
        if (SDL_GetNumJoystickHats(device.joystick) > 0) {
            const auto hat = SDL_GetJoystickHat(device.joystick, 0);
            result.buttons[static_cast<usize>(PadButton::DpadUp)] = (hat & SDL_HAT_UP) != 0;
            result.buttons[static_cast<usize>(PadButton::DpadDown)] = (hat & SDL_HAT_DOWN) != 0;
            result.buttons[static_cast<usize>(PadButton::DpadLeft)] = (hat & SDL_HAT_LEFT) != 0;
            result.buttons[static_cast<usize>(PadButton::DpadRight)] = (hat & SDL_HAT_RIGHT) != 0;
        }
    }
    return result;
}

void Gamepads::poll(Input& input, bool focused) {
    m_focused = focused;
    if (!m_ready) {
        return;
    }
    SDL_UpdateJoysticks();
    for (usize i = 0; i < m_devices.size(); ++i) {
        auto& device = m_devices[i];
        if (device.joystick != nullptr && !SDL_JoystickConnected(device.joystick)) {
            close(device);
            input.setPad(static_cast<s32>(i), {});
        }
    }
    discover();
    if (!m_focused) {
        stop();
    }
    for (usize i = 0; i < m_devices.size(); ++i) {
        input.setPad(static_cast<s32>(i), snapshot(m_devices[i]));
    }
}

bool Gamepads::rumble(s32 pad, u16 low, u16 high, u32 milliseconds, u8 priority) {
    if (!m_ready || !m_focused || pad < 0 || static_cast<usize>(pad) >= m_devices.size()) {
        return false;
    }
    auto& device = m_devices[static_cast<usize>(pad)];
    const u64 now = SDL_GetTicks();
    const bool stopping = milliseconds == 0 || (low == 0 && high == 0);
    if (!stopping && priority < device.rumblePriority && now < device.rumbleUntil) {
        return false;
    }
    if (device.joystick == nullptr || !SDL_JoystickConnected(device.joystick) ||
        !SDL_RumbleJoystick(device.joystick, stopping ? 0 : low, stopping ? 0 : high,
                            stopping ? 0 : milliseconds)) {
        return false;
    }
    device.rumbleUntil = stopping ? 0 : now + milliseconds;
    device.rumblePriority = stopping ? 0 : priority;
    return true;
}

void Gamepads::stop() {
    for (auto& device : m_devices) {
        device.rumbleUntil = 0;
        device.rumblePriority = 0;
        if (device.joystick != nullptr) {
            SDL_RumbleJoystick(device.joystick, 0, 0, 0);
        }
    }
}
} // namespace gdl
