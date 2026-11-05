#include <initializer_list>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/platform/Input.h"

#include "game/config/ControlProfiles.h"
#include "game/config/GameConfig.h"
#include "game/party/PlayInputReader.h"
#include "game/widgets/MenuInput.h"
#include "game/widgets/SceneInput.h"

namespace {

using namespace gdl;
using gdl::game::GameConfig;
using gdl::game::KeyboardScheme;
using gdl::game::kInputSlots;
using gdl::game::MenuInput;
using gdl::game::PlayInputReader;
using gdl::game::SceneInput;

/** One update at 64 a second: a binary fraction, so the gesture clocks add up exactly. */
constexpr f64 kUpdate = 1.0 / 64.0;

/** A connected pad holding `buttons` down. */
PadSnapshot padHolding(std::initializer_list<PadButton> buttons) {
    PadSnapshot pad;
    pad.connected = true;
    for (const PadButton button : buttons) {
        pad.buttons[static_cast<usize>(button)] = true;
    }
    return pad;
}

/** Turns every character toward one facing and notes which slots asked, and from where. */
class RecordingAim final : public PlayInputReader::Aim {
public:
    explicit RecordingAim(Vec3 facing) : m_facing(facing) {}
    std::optional<Vec3> toward(s32 inputSlot, Vec2 cursor) const override {
        m_asked.emplace_back(inputSlot, cursor);
        return m_facing;
    }
    /** The slots that asked since the last take, each with its cursor. */
    std::vector<std::pair<s32, Vec2>> take() { return std::exchange(m_asked, {}); }

private:
    Vec3 m_facing;
    mutable std::vector<std::pair<s32, Vec2>> m_asked;
};

/** Every field of a reading, so that two readings compare whole. */
auto fieldsOf(const MenuInput& menu) {
    return std::tie(menu.devices, menu.pointer, menu.pointerScroll, menu.pointerNormalized,
                    menu.pointerPressed, menu.pointerHeld, menu.pointerBack, menu.up, menu.down,
                    menu.left, menu.right, menu.select, menu.back, menu.start, menu.upHeld,
                    menu.downHeld, menu.leftHeld, menu.rightHeld, menu.typed, menu.erase,
                    menu.escape, menu.buttonPressed);
}

bool sameReading(const MenuInput& left, const MenuInput& right) {
    return fieldsOf(left) == fieldsOf(right);
}

TEST_CASE("each input slot plays from its own devices", "[game][party][controls]") {
    GameConfig config; // automatic: slot 0 has the keyboard and pad 0, slot n pad n
    Input input;
    input.setKey(Key::W, true);
    PadSnapshot stick = padHolding({PadButton::A});
    stick.axes[static_cast<usize>(PadAxis::LeftX)] = 1.0f;
    input.setPad(1, stick);
    input.setPad(2, padHolding({PadButton::B}));
    const SceneInput scene{input, config};
    PlayInputReader reader;
    const RecordingAim aim{Vec3{0.0f, 0.0f, 1.0f}};

    const auto frame = reader.read(scene, kUpdate, aim);
    CHECK(frame[0].move.direction.y > 0.0f); // W walks the keyboard's slot
    CHECK_FALSE(frame[0].attack);
    CHECK_FALSE(frame[0].turbo);
    CHECK(frame[1].move.direction.x > 0.0f); // pad 1's stick
    CHECK(frame[1].attack);
    CHECK(frame[1].attackPressed);
    CHECK_FALSE(frame[1].turbo);
    CHECK_FALSE(frame[2].move.any());
    CHECK_FALSE(frame[2].attack);
    CHECK(frame[2].turbo);
    CHECK_FALSE(frame[3].move.any());
    CHECK_FALSE(frame[3].attack);
    CHECK_FALSE(frame[3].turbo);

    // The settings give the keyboard to slot 3; slot 0 keeps only its pad.
    config.controls[3].device = "keyboard";
    const auto moved = reader.read(scene, kUpdate, aim);
    CHECK_FALSE(moved[0].move.any());
    CHECK(moved[3].move.direction.y > 0.0f);
    CHECK(moved[1].attack);
}

TEST_CASE("Robotron movement works only on the keyboard slot", "[game][party][controls]") {
    GameConfig config;
    config.controls[0].scheme = KeyboardScheme::Robotron;
    config.controls[1].scheme = KeyboardScheme::Robotron; // a pad's slot
    Input input;
    input.setPad(1, padHolding({}));
    const SceneInput scene{input, config};
    PlayInputReader reader;
    const RecordingAim aim{Vec3{0.0f, 0.0f, 1.0f}};

    const auto frame = reader.read(scene, kUpdate, aim);
    CHECK(frame[0].freeMove);
    CHECK_FALSE(frame[1].freeMove);
    CHECK_FALSE(frame[2].freeMove);
    CHECK_FALSE(frame[3].freeMove);

    // The keyboard's slot walks as a tank without the scheme.
    const GameConfig tank;
    CHECK_FALSE(reader.read(SceneInput{input, tank}, kUpdate, aim)[0].freeMove);

    // The keyboard given to slot 1 takes slot 1's scheme with it.
    config.controls[1].device = "keyboard";
    const auto moved = reader.read(scene, kUpdate, aim);
    CHECK(moved[1].freeMove);
    CHECK_FALSE(moved[0].freeMove);
}

TEST_CASE("the cursor aims only for its own slot after the mouse takes over",
          "[game][party][controls][cursor]") {
    const Vec3 facing{0.0f, 0.0f, 1.0f};
    const GameConfig config;
    Input input;
    input.setPointer({0.5f, 0.5f, true, false});
    const SceneInput scene{input, config};
    PlayInputReader reader;
    RecordingAim aim{facing};

    // A pointer that has not moved takes nobody's facing.
    auto frame = reader.read(scene, kUpdate, aim);
    CHECK(aim.take().empty());
    for (const auto& in : frame) {
        CHECK_FALSE(in.aimDirection.has_value());
    }

    // Moved, it turns the keyboard's slot, and only that slot asks where.
    input.beginPoll();
    input.setPointer({0.75f, 0.25f, true, false});
    frame = reader.read(scene, kUpdate, aim);
    auto asked = aim.take();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].first == 0);
    CHECK(asked[0].second == Vec2{0.75f, 0.25f});
    CHECK(frame[0].aimDirection == facing);
    CHECK_FALSE(frame[1].aimDirection.has_value());
    CHECK_FALSE(frame[2].aimDirection.has_value());
    CHECK_FALSE(frame[3].aimDirection.has_value());

    // Once the mouse has taken over, a still cursor goes on aiming.
    input.beginPoll();
    frame = reader.read(scene, kUpdate, aim);
    CHECK(aim.take().size() == 1);
    CHECK(frame[0].aimDirection == facing);

    // A press on the slot's own pad hands its facing back to the stick.
    input.beginPoll();
    input.setPad(0, padHolding({PadButton::A}));
    frame = reader.read(scene, kUpdate, aim);
    CHECK(aim.take().empty());
    CHECK_FALSE(frame[0].aimDirection.has_value());

    // A keyboard given to slot 2 aims for slot 2 alone.
    GameConfig moved;
    moved.controls[2].device = "keyboard";
    PlayInputReader other;
    const auto movedFrame = other.read(SceneInput{input, moved}, kUpdate, aim);
    asked = aim.take();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].first == 2);
    CHECK(movedFrame[2].aimDirection == facing);
    CHECK_FALSE(movedFrame[0].aimDirection.has_value());
}

TEST_CASE("reset drops a tap but keeps cursor ownership", "[game][party][controls][cursor]") {
    const Vec3 facing{0.0f, 0.0f, 1.0f};
    GameConfig config;
    config.play.magicDoubleTapSeconds = 0.25f;
    Input input;
    input.setPointer({0.5f, 0.5f, true, false});
    const SceneInput scene{input, config};
    PlayInputReader reader;
    const RecordingAim aim{facing};
    // Slot 1's potion button on its pad, down or up for an update of `seconds`.
    const auto step = [&](bool magic, f64 seconds) {
        input.beginPoll();
        input.setPad(1, magic ? padHolding({PadButton::X}) : padHolding({}));
        return reader.read(scene, seconds, aim);
    };

    // The mouse takes over the keyboard's slot.
    reader.read(scene, kUpdate, aim);
    input.beginPoll();
    input.setPointer({0.25f, 0.5f, true, false});
    REQUIRE(reader.read(scene, kUpdate, aim)[0].aimDirection == facing);

    // A tap on slot 1's pad waits out the double-tap window before it casts.
    REQUIRE_FALSE(step(true, kUpdate)[1].usePotion);
    REQUIRE_FALSE(step(false, kUpdate)[1].usePotion);
    SECTION("left alone, the tap casts once the window passes") {
        CHECK(step(false, 0.5)[1].usePotion);
    }
    SECTION("reset forgets the tap, and the cursor still aims") {
        reader.reset();
        const auto frame = step(false, 0.5);
        CHECK_FALSE(frame[1].usePotion);
        CHECK(frame[0].aimDirection == facing);
    }
}

TEST_CASE("a frame that is not read does not advance a hold", "[game][party][controls]") {
    GameConfig config;
    config.play.magicHoldSeconds = 0.25f;
    Input input;
    const SceneInput scene{input, config};
    const RecordingAim aim{Vec3{0.0f, 0.0f, 1.0f}};
    PlayInputReader skipping;
    PlayInputReader everyFrame;
    // Slot 1 holds its potion button on its pad through another update.
    const auto hold = [&] {
        input.beginPoll();
        input.setPad(1, padHolding({PadButton::X}));
    };

    hold();
    REQUIRE_FALSE(skipping.read(scene, kUpdate, aim)[1].throwPotion);
    REQUIRE_FALSE(everyFrame.read(scene, kUpdate, aim)[1].throwPotion);
    // Half a second of updates the first reader is not asked about, as with the console up.
    PlayInputReader::Frame last{};
    for (s32 update = 0; update < 8; ++update) {
        hold();
        last = everyFrame.read(scene, 0.0625, aim);
    }
    CHECK(last[1].throwPotion); // held past the quarter second: a throw

    // The first reader's hold is where it was: it throws a quarter second of reading later.
    hold();
    CHECK_FALSE(skipping.read(scene, 0.125, aim)[1].throwPotion);
    hold();
    CHECK_FALSE(skipping.read(scene, 0.0625, aim)[1].throwPotion);
    hold();
    CHECK(skipping.read(scene, 0.0625, aim)[1].throwPotion);
}

TEST_CASE("the menu presses and the movie vote are what scene input reads",
          "[game][party][controls]") {
    GameConfig config;
    config.controls[1].device = "keyboard";
    config.controls[3].device = "none";
    Input input;
    input.setKey(Key::Enter, true);
    input.setKey(Key::Escape, true);
    input.setPad(0, padHolding({PadButton::Start}));
    input.setPad(2, padHolding({PadButton::A, PadButton::DpadLeft}));
    input.setPointer({0.25f, 0.75f, true, true});
    const SceneInput scene{input, config};
    PlayInputReader reader;
    const RecordingAim aim{Vec3{0.0f, 0.0f, 1.0f}};

    const auto frame = reader.read(scene, kUpdate, aim);
    for (s32 slot = 0; slot < static_cast<s32>(kInputSlots); ++slot) {
        CAPTURE(slot);
        const auto& in = frame[static_cast<usize>(slot)];
        CHECK(sameReading(in.menu, scene.slot(slot)));
        CHECK(in.movieSkipPressed == scene.movieVote(slot));
    }
    CHECK(frame[0].movieSkipPressed); // pad 0's Start
    CHECK(frame[1].movieSkipPressed); // the keyboard's Enter
    CHECK(frame[2].movieSkipPressed); // pad 2's A
    CHECK_FALSE(frame[3].movieSkipPressed);
}

} // namespace
