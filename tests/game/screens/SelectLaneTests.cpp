#include <algorithm>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/io/File.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/menu/MenuInput.h"
#include "game/players/CharacterSave.h"
#include "game/players/Progression.h"
#include "game/screens/SelectLane.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** Every printable ASCII glyph is 8 pixels wide on a 10 pixel line. */
BitmapFont fullFont() {
    std::vector<BitmapGlyph> glyphs;
    for (s32 c = '!'; c <= '~'; ++c) {
        glyphs.push_back({c, 8, 0, 0});
    }
    return BitmapFont::fromGlyphs(10, 4, std::move(glyphs));
}

MenuInput press(bool select = false, bool back = false, bool start = false, bool up = false,
                bool down = false, bool left = false, bool right = false) {
    MenuInput input;
    input.select = select;
    input.back = back;
    input.start = start;
    input.up = up;
    input.down = down;
    input.left = left;
    input.right = right;
    return input;
}

struct Fixture {
    BitmapFont font = fullFont();
    test::FakeTexture sheet{64, 64};
    TextPainter painter;
    ClassDataSet classes;
    SaveSlots slots;
    LaneServices services;
    std::vector<SelectSound> sounds;
    SelectLane lane;

    explicit Fixture(std::string_view scratch = "select-lane", bool withSlots = true) {
        painter.setFont(&font, &sheet);
        const auto dir = test::scratchDirectory(scratch);
        writeTextFile(dir / "WAR.json",
                      R"({"fight": [600, 999], "speed": [350, 750], "armor": [300, 700],
                          "magic": [100, 500]})");
        classes.load(dir);
        if (withSlots) {
            slots.open(dir / "saves", 3);
            services.slots = &slots;
        }
        services.classes = &classes;
        services.menuPainter = &painter;
        services.smallPainter = &painter;
        services.initialsPainter = &painter;
        services.largePainter = &painter;
        services.selectTexture = [this](std::string_view) { return &sheet; };
        services.staticTexture = [this](std::string_view) { return &sheet; };
        services.menuTextures.font = &sheet;
        services.playSound = [this](SelectSound sound) { sounds.push_back(sound); };
        lane.reset(1, &services);
    }

    /** Steps the lane once with `input` and a frame where nobody else plays. */
    SelectLane::Result step(const MenuInput& input, s32 ticks = 1,
                            const SelectLane::Frame& frame = {}) {
        return lane.update(input, ticks, frame);
    }

    /** Takes the lane from the New/Load menu through a name to the class picker. */
    void createCharacter() {
        step(press(true)); // New
        REQUIRE(lane.state() == SelectLane::State::NameEntry);
        step(press(false, false, false, true));                      // letter A
        step(press(false, false, false, false, false, false, true)); // enter it
        step(press(true));                                           // accept the end mark
        step(MenuInput{}, NameEntry::kFlashTicks + 1);
        REQUIRE(lane.state() == SelectLane::State::ClassPick);
    }
};

TEST_CASE("a lane joins on activation and leaves from the first menu", "[game][select]") {
    Fixture f;
    REQUIRE_FALSE(f.lane.active());
    REQUIRE(f.lane.x() == SelectLane::kWidth);
    f.lane.activate();
    REQUIRE(f.lane.state() == SelectLane::State::TopMenu);
    REQUIRE(f.lane.selecting());
    REQUIRE(f.step(press(false, true)) == SelectLane::Result::Leave);
    REQUIRE_FALSE(f.lane.active());

    f.lane.activate();
    SelectLane::Frame others;
    others.othersActive = true;
    REQUIRE(f.step(press(false, true), 1, others) == SelectLane::Result::Cleared);
    REQUIRE_FALSE(f.lane.active());
}

TEST_CASE("a new character is named, given a class and locked in", "[game][select]") {
    Fixture f;
    f.lane.activate();
    f.createCharacter();
    REQUIRE(f.lane.save().name == "A");
    REQUIRE(f.lane.pickedClass() == 0);
    f.step(press(false, false, false, false, false, false, true)); // next class
    REQUIRE(f.lane.pickedClass() == 1);
    f.step(press(false, false, false, false, false, true)); // previous class
    f.step(press(false, false, false, false, false, true));
    REQUIRE(f.lane.pickedClass() == kClassCount - 2); // Sumner is skipped while locked
    f.step(press(false, false, false, true));         // next colour
    REQUIRE(f.lane.pickedColor() == 1);
    f.step(press(false, false, false, false, true));
    f.step(press(false, false, false, false, true));
    REQUIRE(f.lane.pickedColor() == 3);
    REQUIRE(f.sounds.back() == SelectSound::ClassChange);

    // Hidden classes cannot be taken.
    REQUIRE(f.step(press(true)) == SelectLane::Result::None);
    REQUIRE(f.lane.state() == SelectLane::State::ClassPick);
    REQUIRE(f.sounds.back() == SelectSound::Buzzer);

    f.step(press(false, false, false, false, false, false, true)); // wraps to the warrior
    REQUIRE(f.lane.pickedClass() == 0);
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::LockedIn);
    REQUIRE_FALSE(f.lane.selecting());
    REQUIRE(f.lane.save().character == 0);
    REQUIRE(f.lane.save().color == 3);
    REQUIRE_FALSE(f.lane.saved());
    REQUIRE(f.sounds.back() == SelectSound::Welcome);
    REQUIRE(f.lane.animating());
    f.step(MenuInput{}, 80);
    REQUIRE_FALSE(f.lane.animating());

    f.step(press(false, false, true)); // Start opens the save menu
    REQUIRE(f.lane.state() == SelectLane::State::SaveMenu);
    f.step(press(true)); // Done
    REQUIRE(f.lane.state() == SelectLane::State::LockedIn);
}

TEST_CASE("backing out of the class picker returns to the first menu", "[game][select]") {
    Fixture f;
    f.lane.activate();
    f.createCharacter();
    f.step(press(false, true));
    REQUIRE(f.lane.state() == SelectLane::State::TopMenu);
    f.step(press(false, true));
    REQUIRE_FALSE(f.lane.active());
}

TEST_CASE("characters save to a slot and load back into the class picker", "[game][select]") {
    Fixture f("select-lane-save");
    f.lane.activate();
    f.createCharacter();
    f.step(press(true)); // lock in the warrior
    f.step(press(false, false, true));
    REQUIRE(f.lane.state() == SelectLane::State::SaveMenu);
    f.step(press(false, false, false, true)); // up from Done: Quit, Change (Load is off)
    f.step(press(false, false, false, true));
    f.step(press(false, false, false, true)); // Save
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::SavePick);
    f.step(press(false, false, false, false, true)); // second slot
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::Saving);
    f.step(MenuInput{}, SelectLane::kOperationStepTicks * 3);
    REQUIRE(f.slots.slot(1).exists);
    REQUIRE(f.slots.slot(1).name == "A");
    REQUIRE(f.lane.saved());
    f.step(MenuInput{}, SelectLane::kNoticeTicks);
    REQUIRE(f.lane.state() == SelectLane::State::SaveMenu);
    REQUIRE(f.lane.slotInUse() == 1U);

    // Saving again over the same slot asks first.
    f.step(press(false, false, false, true));
    f.step(press(false, false, false, true));
    f.step(press(false, false, false, true));
    f.step(press(false, false, false, true));
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::SavePick);
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::OverwriteConfirm);
    f.step(press(true)); // No
    REQUIRE(f.lane.state() == SelectLane::State::SavePick);
    f.step(press(false, true));
    REQUIRE(f.lane.state() == SelectLane::State::SaveMenu);

    // A second lane loads it.
    SelectLane other;
    other.reset(2, &f.services);
    other.activate();
    REQUIRE(other.state() == SelectLane::State::TopMenu);
    other.update(press(true), 1, {}); // Load is preselected once a save exists
    REQUIRE(other.state() == SelectLane::State::LoadPick);
    SelectLane::Frame frame;
    frame.slotsInUse = 1U << 1;
    other.update(press(false, false, false, false, true), 1, frame);
    other.update(press(true), 1, frame); // in use by the first lane
    REQUIRE(other.state() == SelectLane::State::LoadPick);
    other.update(press(true), 1, {});
    REQUIRE(other.state() == SelectLane::State::Loading);
    other.update(MenuInput{}, SelectLane::kOperationStepTicks * 3, {});
    REQUIRE(other.state() == SelectLane::State::Loading);
    other.update(MenuInput{}, SelectLane::kNoticeTicks, {});
    REQUIRE(other.state() == SelectLane::State::ClassPick);
    REQUIRE(other.save().name == "A");
    REQUIRE(other.saved());
    REQUIRE(other.slotInUse() == 1U);
}

TEST_CASE("quitting an unsaved character asks first", "[game][select]") {
    Fixture f("select-lane-quit");
    f.lane.activate();
    f.createCharacter();
    f.step(press(true));
    f.step(press(false, false, true));
    f.step(press(false, false, false, true)); // Quit
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::QuitConfirm);
    f.step(press(true)); // No
    REQUIRE(f.lane.state() == SelectLane::State::SaveMenu);
    f.step(press(false, false, false, true));
    f.step(press(true));
    f.step(press(false, false, false, true)); // Yes
    REQUIRE(f.step(press(true)) == SelectLane::Result::Leave);
    REQUIRE_FALSE(f.lane.active());
}

TEST_CASE("loading with nothing saved shows a notice and drawing emits the lane",
          "[game][select]") {
    Fixture f("select-lane-empty");
    f.lane.activate();
    f.step(press(false, false, false, false, true)); // Load is disabled: the cursor stays
    REQUIRE(f.lane.state() == SelectLane::State::TopMenu);
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::NameEntry);
    f.step(press(false, true));
    REQUIRE(f.lane.state() == SelectLane::State::TopMenu);
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    f.lane.drawImages(canvas);
    f.lane.drawText(canvas, 10);
    canvas.end();
    REQUIRE_FALSE(device.draws.empty());
}

TEST_CASE("name entry letters stay inside the lane", "[game][select]") {
    Fixture f;
    f.lane.activate();
    f.step(press(true)); // New
    REQUIRE(f.lane.state() == SelectLane::State::NameEntry);
    f.step(press(false, false, false, true));                      // letter A
    f.step(press(false, false, false, false, false, false, true)); // enter it
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    f.lane.drawText(canvas, 0);
    canvas.end();
    // Lane 1 spans x 128..256; the taken letter, the pending one and four blanks sit on
    // the letter row.
    f32 minX = 1e9f;
    f32 maxX = -1e9f;
    usize onRow = 0;
    for (const test::RecordedDraw& draw : device.draws) {
        for (const ImmediateVertex& v : draw.vertices) {
            if (v.position.y >= 339.0f && v.position.y <= 351.0f) {
                minX = std::min(minX, v.position.x);
                maxX = std::max(maxX, v.position.x);
                ++onRow;
            }
        }
    }
    REQUIRE(onRow == 6U * NameEntry::kMaxLength); // one quad per letter
    REQUIRE(minX >= 128.0f);
    REQUIRE(maxX <= 256.0f);
}

TEST_CASE("the status box follows the lane's state", "[game][select]") {
    Fixture f("select-lane-box");
    REQUIRE(f.lane.boxMode() == SelectLane::BoxMode::Plain);
    f.lane.activate();
    REQUIRE(f.lane.boxMode() == SelectLane::BoxMode::Plain);
    f.createCharacter();
    REQUIRE(f.lane.boxMode() == SelectLane::BoxMode::Character);
    f.step(press(false, false, false, false, false, false, true));
    REQUIRE(f.lane.boxClass() == 1);
    f.step(press(true));
    REQUIRE(f.lane.boxMode() == SelectLane::BoxMode::Status);
    REQUIRE(f.lane.boxClass() == 1);
    REQUIRE(f.lane.save().health() == kStartingHealth);
    f.step(press(false, false, true));
    REQUIRE(f.lane.boxMode() == SelectLane::BoxMode::Status);
}

} // namespace
