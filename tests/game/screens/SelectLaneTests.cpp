#include <algorithm>
#include <filesystem>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/menu/MenuInput.h"
#include "game/players/CharacterSave.h"
#include "game/players/PowerupEffects.h"
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
    s32 greetedClass = -1;
    s32 greetedColor = -1;
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
        services.playSound = [this](SelectSound sound, const SelectLane& from) {
            sounds.push_back(sound);
            if (sound == SelectSound::Welcome || sound == SelectSound::WelcomeBack) {
                greetedClass = from.save().character;
                greetedColor = from.save().color;
            }
        };
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

TEST_CASE("mouse menus and a typed name create and save a character without click through",
          "[select][mouse]") {
    Fixture f("select-mouse-create");
    f.lane.activate();
    const auto clickAt = [&](const Rect& area,
                             const SelectLane::Frame& frame = SelectLane::Frame{}) {
        MenuInput input;
        input.pointer = Vec2{area.x + area.width / 2, area.y + area.height / 2};
        input.pointerPressed = true;
        return f.step(input, 1, frame);
    };
    const auto clickControl = [&](SelectLane::PointerAction action) {
        const auto targets = f.lane.pointerTargets();
        const auto it = std::ranges::find_if(
            targets, [&](const auto& target) { return target.action == action; });
        REQUIRE(it != targets.end());
        clickAt(it->area);
    };
    clickAt(f.lane.menu().itemArea(0));
    REQUIRE(f.lane.state() == SelectLane::State::NameEntry);
    CHECK(f.lane.nameEntry().name().empty());
    MenuInput typed;
    typed.typed = "az";
    f.step(typed);
    CHECK(f.lane.nameEntry().name() == "AZ");
    MenuInput erase;
    erase.erase = true;
    f.step(erase);
    CHECK(f.lane.nameEntry().name() == "A");
    typed.typed = "b";
    f.step(typed);
    CHECK(f.lane.nameEntry().name() == "AB");
    REQUIRE(f.lane.pointerTargets().empty());
    f.step(press(true));
    f.step({}, NameEntry::kFlashTicks + 1);
    REQUIRE(f.lane.state() == SelectLane::State::ClassPick);
    clickControl(SelectLane::PointerAction::Right);
    CHECK(f.lane.pickedClass() == 1);
    clickControl(SelectLane::PointerAction::Up);
    CHECK(f.lane.pickedColor() == 1);
    clickControl(SelectLane::PointerAction::Select);
    REQUIRE(f.lane.lockedIn());
    CHECK(f.lane.save().name == "AB");
    f.lane.manage();
    clickAt(f.lane.menu().itemArea(0));
    REQUIRE(f.lane.state() == SelectLane::State::SavePick);
    SelectLane::Frame reserved;
    reserved.slotsInUse = {0};
    clickAt(f.lane.menu().itemArea(0), reserved);
    CHECK(f.lane.state() == SelectLane::State::SavePick);
    CHECK_FALSE(f.slots.slot(0).occupied);
    clickAt(f.lane.menu().itemArea(0));
    REQUIRE(f.lane.state() == SelectLane::State::Saving);
    f.step({}, SelectLane::kOperationStepTicks * 3);
    CHECK(f.slots.slot(0).name == "AB");
    f.step({}, SelectLane::kNoticeTicks);
    REQUIRE(f.lane.state() == SelectLane::State::SaveMenu);
    clickAt(f.lane.menu().itemArea(0));
    clickAt(f.lane.menu().itemArea(0));
    REQUIRE(f.lane.state() == SelectLane::State::OverwriteConfirm);
    CHECK(f.lane.menu().selection() == 1); // the slot click never confirms the new page
    clickAt(f.lane.menu().itemArea(1));
    CHECK(f.lane.state() == SelectLane::State::SavePick);
}

TEST_CASE("moving or clicking the mouse never replaces name entry with a character map",
          "[select][mouse]") {
    Fixture f("select-mouse-grid");
    f.lane.activate();
    f.step(press(true));
    MenuInput hover;
    hover.pointer = Vec2{static_cast<f32>(f.lane.x()) + 1, 150};
    f.step(hover);
    CHECK(f.lane.pointerTargets().empty());
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    f.lane.drawText(canvas, 0);
    canvas.end();
    CHECK_FALSE(std::ranges::any_of(device.draws, [&](const auto& draw) {
        return std::ranges::any_of(draw.vertices, [&](const auto& vertex) {
            return vertex.position.y >= 144 && vertex.position.y < 180;
        });
    }));
    hover.pointerPressed = true;
    f.step(hover);
    CHECK(f.lane.nameEntry().name().empty());
    hover.typed = "wasd";
    f.step(hover);
    CHECK(f.lane.nameEntry().name() == "WASD");
}

TEST_CASE("name entry keeps the caption and letters without control help text", "[select][name]") {
    for (const bool keyboard : {false, true}) {
        Fixture f("select-name-no-help");
        test::FakeTexture helpSheet{64, 64};
        TextPainter helpPainter;
        helpPainter.setFont(&f.font, &helpSheet);
        f.services.smallPainter = &helpPainter;
        f.services.keyboardLane = keyboard ? f.lane.index() : -1;
        f.lane.activate();
        f.step(press(true));
        REQUIRE(f.lane.state() == SelectLane::State::NameEntry);
        MenuInput typed;
        typed.typed = "ace";
        f.step(typed);
        CHECK(f.lane.nameEntry().name() == "ACE");

        test::FakeRenderDevice device;
        Canvas canvas;
        canvas.begin(device, Mat4{1});
        f.lane.drawText(canvas, 0);
        canvas.end();
        CHECK_FALSE(std::ranges::any_of(
            device.draws, [&](const auto& draw) { return draw.texture == &helpSheet; }));
        bool caption = false;
        bool letters = false;
        for (const auto& draw : device.draws) {
            for (const auto& vertex : draw.vertices) {
                caption = caption || (vertex.position.y >= 64 && vertex.position.y < 80);
                letters = letters || vertex.position.y >= 340;
            }
        }
        CHECK(caption);
        CHECK(letters);
        f.step(press(true));
        f.step({}, NameEntry::kFlashTicks + 1);
        CHECK(f.lane.state() == SelectLane::State::ClassPick);
        CHECK(f.lane.save().name == "ACE");
    }
}

TEST_CASE("stationary mouse preserves keyboard focus and unavailable load rows stay inert",
          "[select][mouse]") {
    Fixture f("select-mouse-focus");
    CharacterSave saved;
    saved.name = "SAVED";
    REQUIRE(f.slots.write(0, saved));
    f.lane.activate();
    const auto area = f.lane.menu().itemArea(0);
    MenuInput input;
    input.pointer = Vec2{area.x + 1, area.y + 1};
    f.step(input);
    CHECK(f.lane.menu().selection() == 0);
    input.down = true;
    f.step(input);
    CHECK(f.lane.menu().selection() == 1);
    input.down = false;
    f.step(input);
    CHECK(f.lane.menu().selection() == 1);
    input.select = true;
    f.step(input);
    REQUIRE(f.lane.state() == SelectLane::State::LoadPick);
    const auto empty = f.lane.menu().itemArea(1);
    input.select = false;
    input.pointerPressed = true;
    input.pointer = Vec2{empty.x + 1, empty.y + 1};
    f.step(input);
    CHECK(f.lane.state() == SelectLane::State::LoadPick);
    CHECK_FALSE(f.lane.reservedSlot());
}

TEST_CASE("name cheats enter through the ordinary character selection flow",
          "[game][select][cheats]") {
    Fixture f;
    f.lane.activate();
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::NameEntry);
    MenuInput name;
    SECTION("powerup names apply only when the class is committed") {
        name.typed = "invuln";
        f.step(name);
        f.step({}, NameEntry::kFlashTicks + 1);
        REQUIRE(f.lane.state() == SelectLane::State::ClassPick);
        CHECK(f.lane.save().progress().inventory.powerupCount() == 0);
        f.step(press(true));
        REQUIRE(f.lane.lockedIn());
        CHECK(f.lane.save().name == "INVULN");
        CHECK((PowerupEffects::of(f.lane.save().progress().inventory).armor &
               powerup::kInvulnerable) != 0);
    }
    SECTION("costume names force the authored class and colour before greeting") {
        name.typed = "ICE600";
        f.step(name);
        f.step({}, NameEntry::kFlashTicks + 1);
        f.step(press(true));
        REQUIRE(f.lane.lockedIn());
        CHECK(f.lane.save().character == 4);
        CHECK(f.lane.save().color == 2);
        CHECK(f.lane.pickedClass() == 4);
        CHECK(f.lane.pickedColor() == 2);
        CHECK(f.greetedClass == 4);
        CHECK(f.greetedColor == 2);
    }
}

TEST_CASE("select lanes retain actions without drawing mapped button prompts",
          "[game][select][prompts]") {
    Fixture f;
    std::vector<std::string> actions;
    f.services.controlLabels = [&](s32 player, std::string_view action) {
        CHECK(player == 1);
        actions.emplace_back(action);
        return "F2";
    };
    f.services.staticTexture = {};
    f.lane.activate();
    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    f.lane.drawText(canvas, 0);
    canvas.end();
    CHECK(actions.empty());
}

TEST_CASE("select lanes remove footer labels and their invisible mouse targets",
          "[game][select][prompts][mouse]") {
    Fixture f("select-no-footer");
    f.lane.activate();
    test::FakeTexture helpSheet{64, 64};
    TextPainter helpPainter;
    helpPainter.setFont(&f.font, &helpSheet);
    f.services.smallPainter = &helpPainter;
    REQUIRE(f.lane.pointerTargets().empty());
    for (const f32 y : {255.0f, 275.0f}) {
        MenuInput mouse;
        mouse.pointer = Vec2{static_cast<f32>(f.lane.x()) + 65, y};
        mouse.pointerPressed = true;
        CHECK(f.step(mouse) == SelectLane::Result::None);
        CHECK(f.lane.state() == SelectLane::State::TopMenu);
        test::FakeRenderDevice device;
        Canvas canvas;
        canvas.begin(device, Mat4{1});
        f.lane.drawText(canvas, 0);
        canvas.end();
        CHECK_FALSE(device.draws.empty());
        CHECK_FALSE(std::ranges::any_of(device.draws, [&](const auto& draw) {
            return draw.texture == &helpSheet || draw.texture == &device.whiteTexture();
        }));
    }
    CHECK(f.step(press(false, true)) == SelectLane::Result::Leave);
}

TEST_CASE("a class is mouse-selected on its portrait instead of the removed footer",
          "[game][select][mouse]") {
    Fixture f("select-portrait-confirm");
    f.lane.activate();
    f.createCharacter();
    const auto targets = f.lane.pointerTargets();
    const auto select = std::ranges::find_if(targets, [](const auto& target) {
        return target.action == SelectLane::PointerAction::Select;
    });
    REQUIRE(select != targets.end());
    CHECK(select->area.y == 28);
    CHECK(select->area.bottom() == 162);
    MenuInput click;
    click.pointerPressed = true;
    click.pointer = Vec2{static_cast<f32>(f.lane.x()) + 65, 255};
    f.step(click);
    CHECK(f.lane.state() == SelectLane::State::ClassPick);
    click.pointer =
        Vec2{select->area.x + select->area.width / 2, select->area.y + select->area.height / 2};
    f.step(click);
    CHECK(f.lane.lockedIn());
}

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

TEST_CASE("a character in play resumes its lane locked in, and Start still changes it",
          "[game][select]") {
    Fixture f;
    CharacterSave save;
    save.name = "PLAYING";
    save.character = 2;
    save.color = 3;
    f.lane.resume(save, std::optional<usize>{4});
    CHECK(f.lane.lockedIn());
    CHECK(f.lane.save().name == "PLAYING");
    CHECK(f.lane.slotInUse() == std::optional<usize>{4});
    CHECK(f.lane.saved());
    CHECK(f.lane.boxClass() == 2);
    CHECK(f.lane.boxColor() == 3);
    // Unsaved, it is resumed as such.
    f.lane.resume(save, std::nullopt);
    CHECK_FALSE(f.lane.saved());
    // Start takes it back to its save menu, as any locked-in lane.
    MenuInput start;
    start.start = true;
    f.step(start);
    CHECK(f.lane.state() == SelectLane::State::SaveMenu);
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
    REQUIRE(f.greetedClass == 0); // the greeting can name the character
    REQUIRE(f.greetedColor == 3);
    REQUIRE(f.lane.animating());
    f.step(MenuInput{}, 80);
    REQUIRE_FALSE(f.lane.animating());

    f.step(press(false, false, true)); // Start opens the save menu
    REQUIRE(f.lane.state() == SelectLane::State::SaveMenu);
    f.step(press(true)); // Done
    REQUIRE(f.lane.state() == SelectLane::State::LockedIn);
}

TEST_CASE("a typed name is taken straight from the keyboard", "[game][select]") {
    Fixture f;
    f.lane.activate();
    f.step(press(true)); // New
    REQUIRE(f.lane.typing());
    MenuInput erase;
    erase.erase = true;
    f.step(erase); // nothing to erase, and nothing else happens
    REQUIRE(f.lane.typing());
    MenuInput escape;
    escape.escape = true;
    f.step(escape); // Escape leaves the name for the menu
    REQUIRE(f.lane.state() == SelectLane::State::TopMenu);
    REQUIRE_FALSE(f.lane.typing());

    f.step(press(true));
    MenuInput typed;
    typed.typed = "cj";
    f.step(typed);
    REQUIRE(f.lane.nameEntry().name() == "CJ");
    REQUIRE(f.sounds.back() == SelectSound::LetterAccept);
    f.step(erase);
    REQUIRE(f.lane.nameEntry().name() == "C");
    REQUIRE(f.sounds.back() == SelectSound::CursorHorizontal);
    typed.typed = "j";
    f.step(typed);
    f.step(press(true)); // Enter takes it
    f.step(MenuInput{}, NameEntry::kFlashTicks + 1);
    REQUIRE(f.lane.state() == SelectLane::State::ClassPick);
    REQUIRE(f.lane.save().name == "CJ");
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
    frame.slotsInUse = {1};
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

TEST_CASE("changing the selected class restores its wallet after browsing without transfer",
          "[game][select][class-wallet]") {
    Fixture f("select-class-wallet");
    CharacterSave save;
    save.name = "GOLD";
    save.gold = 800;
    save.classes[1].gold = 120;
    f.lane.resume(save, std::nullopt);
    f.lane.manage();
    const auto chooseChange = [&] {
        f.step(press(false, false, false, true)); // Quit.
        f.step(press(false, false, false, true)); // Change; no files to load.
        f.step(press(true));
        REQUIRE(f.lane.state() == SelectLane::State::ClassPick);
    };
    chooseChange();
    f.step(press(false, false, false, false, false, false, true));
    CHECK(f.lane.save().gold == 800); // Browsing does not commit the class.
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::SaveMenu);
    CHECK(f.lane.save().character == 1);
    CHECK(f.lane.save().gold == 120);
    chooseChange();
    f.step(press(false, false, false, false, false, true));
    f.step(press(true));
    CHECK(f.lane.save().character == 0);
    CHECK(f.lane.save().gold == 800);
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

TEST_CASE("a save refused at execution keeps the character and its previous slot intact",
          "[game][select][post-shop]") {
    Fixture f("select-save-refused");
    CharacterSave original;
    original.name = "ORIGIN";
    original.gold = 777;
    REQUIRE(f.slots.write(2, original));
    f.lane.resume(original, 2);
    f.lane.manage();
    for (s32 i = 0; i < 4; ++i) {
        f.step(press(false, false, false, true)); // Done -> Save.
    }
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::SavePick);
    f.step(press(true));
    REQUIRE(f.lane.reservedSlot() == 0);
    SelectLane::Frame conflict;
    SECTION("another lane claimed the slot") {
        conflict.slotsInUse = {0};
    }
    SECTION("the filesystem rejects replacement") {
        REQUIRE(std::filesystem::create_directory(f.slots.path(0)));
    }
    f.step({}, SelectLane::kOperationStepTicks * 3, conflict);
    CHECK_FALSE(f.slots.slot(0).exists);
    CHECK(f.lane.slotInUse() == 2);
    CHECK(f.lane.save().toJson() == original.toJson());
    f.step({}, SelectLane::kNoticeTicks, conflict);
    CHECK(f.lane.state() == SelectLane::State::SaveMenu);
    CHECK_FALSE(f.lane.reservedSlot().has_value());
}

TEST_CASE("save slot ownership keeps large slot ids distinct from low slots", "[game][select]") {
    Fixture f("select-high-slots");
    REQUIRE(f.slots.open(f.slots.directory(), 40));
    CharacterSave original;
    original.name = "ORIGIN";
    f.lane.resume(original, std::nullopt);
    f.lane.manage();
    for (s32 i = 0; i < 3; ++i) {
        f.step(press(false, false, false, true)); // Done -> Save, with no files to load.
    }
    f.step(press(true));
    REQUIRE(f.lane.state() == SelectLane::State::SavePick);
    SelectLane::Frame other;
    other.slotsInUse = {32};
    SECTION("an occupied high slot does not reserve slot zero") {
        f.step(press(true), 1, other);
        REQUIRE(f.lane.state() == SelectLane::State::Saving);
        f.step({}, SelectLane::kOperationStepTicks * 3, other);
        CHECK(f.lane.slotInUse() == 0);
        CHECK(f.slots.slot(0).name == original.name);
    }
    SECTION("a high slot cannot be selected while another lane holds it") {
        for (s32 i = 0; i < 32; ++i) {
            f.step(press(false, false, false, false, true), 1, other);
        }
        f.step(press(true), 1, other);
        CHECK(f.lane.state() == SelectLane::State::SavePick);
        CHECK_FALSE(f.lane.reservedSlot());
        CHECK(f.sounds.back() == SelectSound::Buzzer);
    }
    SECTION("a high slot claimed during an operation stays protected") {
        for (s32 i = 0; i < 32; ++i) {
            f.step(press(false, false, false, false, true));
        }
        f.step(press(true));
        REQUIRE(f.lane.reservedSlot() == 32);
        f.step({}, SelectLane::kOperationStepTicks * 3, other);
        CHECK_FALSE(f.lane.slotInUse());
        CHECK_FALSE(f.slots.slot(32).exists);
    }
}

} // namespace
