#include <array>
#include <memory>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/platform/Gamepads.h"

namespace {
using namespace gdl;

struct Motors {
    u16 low = 0;
    u16 high = 0;
    s32 calls = 0;
};

struct VirtualPad {
    std::unique_ptr<Motors> motors = std::make_unique<Motors>();
    u32 id = 0;
    SDL_Joystick* joystick = nullptr;
    explicit VirtualPad(bool gamepad = true, bool feedback = true, u16 product = 0x1234) {
        SDL_VirtualJoystickDesc desc{};
        SDL_INIT_INTERFACE(&desc);
        desc.type =
            static_cast<u16>(gamepad ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_FLIGHT_STICK);
        desc.vendor_id = 0xFEED;
        desc.product_id = product;
        desc.name = product == 0x1234 ? "GDL virtual feedback test" : "Other virtual device";
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.nhats = 1;
        desc.button_mask = (1U << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        desc.axis_mask = (1U << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.userdata = motors.get();
        if (feedback) {
            desc.Rumble = [](void* data, u16 low, u16 high) {
                auto& state = *static_cast<Motors*>(data);
                state.low = low;
                state.high = high;
                ++state.calls;
                return true;
            };
        }
        id = SDL_AttachVirtualJoystick(&desc);
        REQUIRE(id != 0);
        joystick = SDL_OpenJoystick(id);
        REQUIRE(joystick != nullptr);
    }
    ~VirtualPad() {
        SDL_CloseJoystick(joystick);
        if (id != 0) {
            SDL_DetachVirtualJoystick(id);
        }
    }
    GDL_NON_COPYABLE_NON_MOVABLE(VirtualPad);
    void disconnect() {
        REQUIRE(SDL_DetachVirtualJoystick(id));
        id = 0;
    }
};

std::vector<s32> testSlots(const Input& input) {
    std::vector<s32> result;
    for (s32 i = 0; i < Input::kMaxPads; ++i) {
        if (const auto* pad = input.padDevice(i);
            pad != nullptr && pad->name == "GDL virtual feedback test") {
            result.push_back(i);
        }
    }
    return result;
}

TEST_CASE("controller GUIDs retain existing Windows and Linux assignments", "[input][rumble]") {
    std::array<u8, 16> guid{3, 0, 0xAB, 0xCD, 0x5E, 4, 0, 0, 0x8E, 2, 0, 0, 1, 0, 'x', 1};
    CHECK(Gamepads::configurationGuid(guid, "Xbox", true) == "78696e70757401000000000000000000");
    guid[14] = 'h';
    CHECK(Gamepads::configurationGuid(guid, "USB", true) == "030000005e0400008e02000000000000");
    CHECK(Gamepads::configurationGuid(guid, "USB", false) == "030000005e0400008e02000001000000");
    guid = {};
    guid[0] = 5;
    CHECK(Gamepads::configurationGuid(guid, "Generic", true) == "0500000047656e657269630000000000");
    CHECK(Gamepads::configurationGuid(guid, "Generic", false) ==
          "0500000047656e657269630000000000");
}

TEST_CASE("one handle supplies mapped input and independently timed controller motors",
          "[input][rumble]") {
    Gamepads devices;
    const VirtualPad first;
    const VirtualPad second;
    Input input;
    devices.poll(input, true);
    const auto slots = testSlots(input);
    REQUIRE(slots.size() == 2);
    CHECK(input.padDevice(slots[0])->rumbleSupported);
    CHECK(input.padDevice(slots[1])->rumbleSupported);
    REQUIRE(SDL_SetJoystickVirtualButton(second.joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
    REQUIRE(SDL_SetJoystickVirtualAxis(second.joystick, SDL_GAMEPAD_AXIS_LEFTX, -32768));
    REQUIRE(SDL_SetJoystickVirtualAxis(second.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767));
    input.beginPoll();
    devices.poll(input, true);
    CHECK_FALSE(input.isPadButtonDown(slots[0], PadButton::A));
    CHECK(input.wasPadButtonPressed(slots[1], PadButton::A));
    CHECK(input.padAxis(slots[1], PadAxis::LeftX) == Catch::Approx(-1));
    CHECK(input.padAxis(slots[1], PadAxis::LeftTrigger) == Catch::Approx(1));
    CHECK(input.wasPadButtonPressed(slots[1], PadButton::LeftTrigger));
    REQUIRE(devices.rumble(slots[1], 0xFFFF, 0, 1000));
    CHECK(second.motors->low == 0xFFFF);
    CHECK(first.motors->low == 0);
    REQUIRE(devices.rumble(slots[0], 10000, 20000, 1000));
    CHECK(first.motors->high == 20000);
    // New feedback replaces, rather than queues behind, the old duration.
    REQUIRE(devices.rumble(slots[1], 0xFFFF, 0, 10));
    SDL_Delay(40);
    devices.poll(input, true);
    CHECK(second.motors->low == 0);
    CHECK(first.motors->low == 10000);
    devices.stop();
    CHECK(first.motors->low == 0);
    CHECK(first.motors->high == 0);
    CHECK_FALSE(devices.rumble(-1, 1, 1, 10));
    CHECK_FALSE(devices.rumble(Input::kMaxPads, 1, 1, 10));
}

TEST_CASE("focus loss stops feedback and never replays it on returning", "[input][rumble]") {
    Gamepads devices;
    const VirtualPad pad;
    Input input;
    devices.poll(input, true);
    const auto slots = testSlots(input);
    REQUIRE(slots.size() == 1);
    REQUIRE(devices.rumble(slots[0], 50000, 30000, 1000));
    devices.poll(input, false);
    CHECK(pad.motors->low == 0);
    CHECK(pad.motors->high == 0);
    CHECK_FALSE(devices.rumble(slots[0], 50000, 0, 1000));
    devices.poll(input, true);
    CHECK(pad.motors->low == 0);
}

TEST_CASE("light hit pulses cannot replace damage feedback or leak priority across stops",
          "[input][rumble][melee-rumble]") {
    Gamepads devices;
    const VirtualPad first;
    const VirtualPad second;
    Input input;
    devices.poll(input, true);
    const auto slots = testSlots(input);
    REQUIRE(slots.size() == 2);
    REQUIRE(devices.rumble(slots[0], 0, 0x8000, 90));
    REQUIRE(devices.rumble(slots[0], 0xFFFF, 0, 1000, 1));
    const s32 calls = first.motors->calls;
    CHECK_FALSE(devices.rumble(slots[0], 0, 0x8000, 90));
    CHECK(first.motors->calls == calls);
    CHECK(first.motors->low == 0xFFFF);
    CHECK(first.motors->high == 0);
    REQUIRE(devices.rumble(slots[1], 0, 0x8000, 90));
    CHECK(second.motors->high == 0x8000);
    SECTION("expiry") {
        REQUIRE(devices.rumble(slots[0], 0xFFFF, 0, 10, 1));
        SDL_Delay(40);
        devices.poll(input, true);
        CHECK(first.motors->low == 0);
    }
    SECTION("pause or scene exit") {
        devices.stop();
        CHECK(first.motors->low == 0);
        CHECK(second.motors->high == 0);
    }
    SECTION("focus loss") {
        devices.poll(input, false);
        CHECK_FALSE(devices.rumble(slots[0], 0, 0x8000, 90));
        CHECK(first.motors->low == 0);
        devices.poll(input, true);
        CHECK(first.motors->low == 0);
    }
    SECTION("explicit zero pulse bypasses priority") {
        REQUIRE(devices.rumble(slots[0], 0, 0, 0));
        CHECK(first.motors->low == 0);
    }
    REQUIRE(devices.rumble(slots[0], 0, 0x8000, 90));
    CHECK(first.motors->low == 0);
    CHECK(first.motors->high == 0x8000);
}

TEST_CASE("disconnecting one controller neither renumbers nor vibrates the other",
          "[input][rumble]") {
    Gamepads devices;
    VirtualPad first;
    const VirtualPad second;
    Input input;
    devices.poll(input, true);
    const auto slots = testSlots(input);
    REQUIRE(slots.size() == 2);
    REQUIRE(devices.rumble(slots[0], 65535, 0, 1000, 1));
    first.disconnect();
    input.beginPoll();
    devices.poll(input, true);
    CHECK_FALSE(input.isPadConnected(slots[0]));
    CHECK(input.isPadConnected(slots[1]));
    CHECK_FALSE(devices.rumble(slots[0], 65535, 0, 1000));
    CHECK(second.motors->low == 0);
    const VirtualPad other(true, true, 0x5678);
    devices.poll(input, true);
    // A different model must not occupy the unplugged twin's identity slot and alter
    // the occurrence used to route the surviving player's input and vibration.
    CHECK_FALSE(input.isPadConnected(slots[0]));
    CHECK(input.padSlot(slots[0])->guid == input.padDevice(slots[1])->guid);
    const VirtualPad replacement;
    devices.poll(input, true);
    CHECK(input.isPadConnected(slots[0]));
    REQUIRE(devices.rumble(slots[0], 65535, 0, 1000));
    CHECK(replacement.motors->low == 65535);
    CHECK(second.motors->low == 0);
    CHECK(other.motors->low == 0);
}

TEST_CASE("unmapped joysticks retain numbered buttons and tolerate missing motors",
          "[input][rumble]") {
    Gamepads devices;
    const VirtualPad pad(false, false);
    REQUIRE_FALSE(SDL_IsGamepad(pad.id));
    Input input;
    REQUIRE(SDL_SetJoystickVirtualButton(pad.joystick, 11, true));
    REQUIRE(SDL_SetJoystickVirtualHat(pad.joystick, 0, SDL_HAT_LEFT));
    devices.poll(input, true);
    const auto slots = testSlots(input);
    REQUIRE(slots.size() == 1);
    CHECK_FALSE(input.padDevice(slots[0])->rumbleSupported);
    CHECK(input.isPadButtonDown(slots[0], PadButton::Button12));
    CHECK(input.isPadButtonDown(slots[0], PadButton::DpadLeft));
    CHECK_FALSE(devices.rumble(slots[0], 65535, 0, 1000));
}

TEST_CASE("controller snapshots expose motor capability without starting feedback",
          "[input][rumble][rumble-capability]") {
    const bool mapped = GENERATE(false, true);
    const bool feedback = GENERATE(false, true);
    Gamepads devices;
    VirtualPad pad(mapped, feedback);
    Input input;
    devices.poll(input, true);
    const auto slots = testSlots(input);
    REQUIRE(slots.size() == 1);
    REQUIRE(input.padDevice(slots[0]));
    CHECK(input.padDevice(slots[0])->rumbleSupported == feedback);
    CHECK(pad.motors->calls == 0);
    Input buffered;
    buffered.accumulate(input);
    REQUIRE(buffered.padDevice(slots[0]));
    CHECK(buffered.padDevice(slots[0])->rumbleSupported == feedback);
    pad.disconnect();
    input.beginPoll();
    devices.poll(input, true);
    CHECK_FALSE(input.padDevice(slots[0]));
    REQUIRE(input.padSlot(slots[0]));
    CHECK_FALSE(input.padSlot(slots[0])->rumbleSupported);
}

TEST_CASE("controller shutdown explicitly stops active motors", "[input][rumble]") {
    REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
    {
        const VirtualPad pad;
        {
            Gamepads devices;
            Input input;
            devices.poll(input, true);
            const auto slots = testSlots(input);
            REQUIRE(slots.size() == 1);
            REQUIRE(devices.rumble(slots[0], 65535, 0, 1000));
            CHECK(pad.motors->low == 65535);
        }
        CHECK(pad.motors->low == 0);
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
} // namespace
