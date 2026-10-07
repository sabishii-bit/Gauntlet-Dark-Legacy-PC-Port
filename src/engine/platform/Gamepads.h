#pragma once

#include <array>
#include <string>
#include <string_view>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/platform/Input.h"

struct SDL_Joystick;
struct SDL_Gamepad;

namespace gdl {

/** Controller input and motors share one device handle. Slots stay stable while other
 * controllers connect/disconnect; closing a handle always stops its motors first. */
class Gamepads {
public:
    Gamepads();
    ~Gamepads();
    GDL_NON_COPYABLE_NON_MOVABLE(Gamepads);

    void poll(Input& input, bool focused);
    bool rumble(s32 pad, u16 low, u16 high, u32 milliseconds);
    void stop();

    /** Retain the GUID format used by existing controller profiles. SDL adds a name CRC
     * and driver signature that are not part of that persisted identity. */
    static std::string configurationGuid(const std::array<u8, 16>& guid, std::string_view name,
                                         bool windows);

private:
    struct Device {
        SDL_Joystick* joystick = nullptr;
        SDL_Gamepad* gamepad = nullptr;
        u32 id = 0;
        std::string guid;
        std::string path;
    };
    static void close(Device& device);
    static PadSnapshot snapshot(const Device& device);
    void discover();
    std::array<Device, Input::kMaxPads> m_devices;
    bool m_ready = false;
    bool m_focused = false;
};

} // namespace gdl
