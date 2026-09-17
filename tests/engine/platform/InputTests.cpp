#include <catch2/catch_test_macros.hpp>

#include "engine/platform/Input.h"

namespace {

using namespace gdl;

TEST_CASE("keys report down, pressed and released edges across polls", "[platform][input]") {
    Input input;
    REQUIRE_FALSE(input.isKeyDown(Key::Space));

    input.beginPoll();
    input.setKey(Key::Space, true);
    REQUIRE(input.isKeyDown(Key::Space));
    REQUIRE(input.wasKeyPressed(Key::Space));
    REQUIRE_FALSE(input.wasKeyReleased(Key::Space));

    input.beginPoll();
    input.setKey(Key::Space, true);
    REQUIRE(input.isKeyDown(Key::Space));
    REQUIRE_FALSE(input.wasKeyPressed(Key::Space));

    input.beginPoll();
    input.setKey(Key::Space, false);
    REQUIRE_FALSE(input.isKeyDown(Key::Space));
    REQUIRE(input.wasKeyReleased(Key::Space));
}

TEST_CASE("keys and pad buttons round-trip through their names", "[platform][input]") {
    REQUIRE(keyName(Key::Enter) == "Enter");
    REQUIRE(keyName(Key::W) == "W");
    REQUIRE(keyFromName("enter") == Key::Enter);
    REQUIRE(keyFromName("BACKSPACE") == Key::Backspace);
    REQUIRE_FALSE(keyFromName("NoSuchKey").has_value());
    REQUIRE_FALSE(keyFromName("").has_value());
    REQUIRE(padButtonName(PadButton::DpadUp) == "DpadUp");
    REQUIRE(padButtonFromName("dpadup") == PadButton::DpadUp);
    REQUIRE(padButtonFromName("start") == PadButton::Start);
    REQUIRE_FALSE(padButtonFromName("Select").has_value());
    for (usize i = 1; i < static_cast<usize>(Key::Count); ++i) {
        const auto key = static_cast<Key>(i);
        REQUIRE(keyFromName(keyName(key)) == key);
    }
    for (usize i = 0; i < static_cast<usize>(PadButton::Count); ++i) {
        const auto button = static_cast<PadButton>(i);
        REQUIRE(padButtonFromName(padButtonName(button)) == button);
    }
}

TEST_CASE("the Unknown and Count sentinels are ignored", "[platform][input]") {
    Input input;
    input.setKey(Key::Unknown, true);
    input.setKey(Key::Count, true);
    REQUIRE_FALSE(input.isKeyDown(Key::Unknown));
    REQUIRE_FALSE(input.isKeyDown(Key::Count));
}

TEST_CASE("gamepad snapshots are exposed per pad with edge detection", "[platform][input]") {
    Input input;
    PadSnapshot snapshot;
    snapshot.connected = true;
    snapshot.buttons[static_cast<usize>(PadButton::A)] = true;
    snapshot.axes[static_cast<usize>(PadAxis::LeftX)] = -0.5f;

    input.beginPoll();
    input.setPad(1, snapshot);
    REQUIRE(input.isPadConnected(1));
    REQUIRE_FALSE(input.isPadConnected(0));
    REQUIRE(input.isPadButtonDown(1, PadButton::A));
    REQUIRE(input.wasPadButtonPressed(1, PadButton::A));
    REQUIRE_FALSE(input.isPadButtonDown(1, PadButton::B));
    REQUIRE(input.padAxis(1, PadAxis::LeftX) == -0.5f);

    input.beginPoll();
    input.setPad(1, snapshot);
    REQUIRE(input.isPadButtonDown(1, PadButton::A));
    REQUIRE_FALSE(input.wasPadButtonPressed(1, PadButton::A));
}

TEST_CASE("out-of-range pad indices are harmless", "[platform][input]") {
    Input input;
    PadSnapshot snapshot;
    snapshot.connected = true;
    input.setPad(-1, snapshot);
    input.setPad(Input::kMaxPads, snapshot);
    REQUIRE_FALSE(input.isPadConnected(-1));
    REQUIRE_FALSE(input.isPadConnected(Input::kMaxPads));
    REQUIRE_FALSE(input.isPadButtonDown(99, PadButton::Start));
    REQUIRE(input.padAxis(99, PadAxis::RightY) == 0.0f);
}

} // namespace
