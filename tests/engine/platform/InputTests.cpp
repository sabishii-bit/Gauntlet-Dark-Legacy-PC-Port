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

TEST_CASE("a key tapped between polls counts as down for the poll after it", "[platform][input]") {
    Input input;
    // The press arrived and the key was up again by the time the state was read.
    input.beginPoll();
    input.latchKey(Key::Left);
    input.setKey(Key::Left, false);
    REQUIRE(input.isKeyDown(Key::Left));
    REQUIRE(input.wasKeyPressed(Key::Left));
    REQUIRE_FALSE(input.wasKeyReleased(Key::Left));
    // The next poll sees the release edge, and nothing lingers.
    input.beginPoll();
    input.setKey(Key::Left, false);
    REQUIRE_FALSE(input.isKeyDown(Key::Left));
    REQUIRE(input.wasKeyReleased(Key::Left));
    input.beginPoll();
    REQUIRE_FALSE(input.wasKeyReleased(Key::Left));
    // A latch on a key that stays down is just a press.
    input.beginPoll();
    input.latchKey(Key::Left);
    input.setKey(Key::Left, true);
    REQUIRE(input.wasKeyPressed(Key::Left));
    input.beginPoll();
    input.setKey(Key::Left, true);
    REQUIRE(input.isKeyDown(Key::Left));
    REQUIRE_FALSE(input.wasKeyPressed(Key::Left));
    input.latchKey(Key::Unknown); // ignored
}

TEST_CASE("typed characters are kept until the next poll", "[platform][input]") {
    Input input;
    REQUIRE(input.typedText().empty());
    input.beginPoll();
    input.addTypedChar('a');
    input.addTypedChar(0x20AC);
    REQUIRE(input.typedText().size() == 2);
    REQUIRE(input.typedText()[0] == 'a');
    REQUIRE(input.typedText()[1] == 0x20AC);
    input.beginPoll();
    REQUIRE(input.typedText().empty());
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
