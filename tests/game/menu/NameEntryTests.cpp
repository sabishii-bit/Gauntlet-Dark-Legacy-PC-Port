#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "game/menu/MenuInput.h"
#include "game/menu/NameEntry.h"

namespace {

using namespace gdl;
using namespace gdl::game;

MenuInput press(bool up = false, bool down = false, bool left = false, bool right = false,
                bool select = false) {
    MenuInput input;
    input.up = up;
    input.down = down;
    input.left = left;
    input.right = right;
    input.select = select;
    return input;
}

TEST_CASE("the letter cycle runs through letters, underscore, digits and the end mark",
          "[game][menu][name]") {
    REQUIRE(NameEntry::nextLetter('A') == 'B');
    REQUIRE(NameEntry::nextLetter('Z') == '_');
    REQUIRE(NameEntry::nextLetter('_') == '0');
    REQUIRE(NameEntry::nextLetter('9') == NameEntry::kEndMark);
    REQUIRE(NameEntry::nextLetter(NameEntry::kEndMark) == 'A');
    REQUIRE(NameEntry::previousLetter('A') == NameEntry::kEndMark);
    REQUIRE(NameEntry::previousLetter(NameEntry::kEndMark) == '9');
    REQUIRE(NameEntry::previousLetter('0') == '_');
    REQUIRE(NameEntry::previousLetter('_') == 'Z');
    REQUIRE(NameEntry::previousLetter('C') == 'B');
}

TEST_CASE("letters are picked, entered and removed", "[game][menu][name]") {
    NameEntry entry;
    entry.begin("");
    REQUIRE(entry.editing());
    REQUIRE(entry.pendingLetter() == NameEntry::kEndMark);
    REQUIRE(entry.update(press(true), 1) == NameEntry::Event::LetterChanged);
    REQUIRE(entry.pendingLetter() == 'A');
    REQUIRE(entry.update(press(true), 1) == NameEntry::Event::LetterChanged);
    REQUIRE(entry.pendingLetter() == 'B');
    REQUIRE(entry.update(press(false, false, false, true), 1) == NameEntry::Event::LetterAdded);
    REQUIRE(entry.name() == "B");
    REQUIRE(entry.pendingLetter() == NameEntry::kEndMark);
    REQUIRE(entry.update(press(false, true), 1) == NameEntry::Event::LetterChanged);
    REQUIRE(entry.pendingLetter() == '9');
    REQUIRE(entry.update(press(false, false, false, false, true), 1) ==
            NameEntry::Event::LetterAdded);
    REQUIRE(entry.name() == "B9");
    REQUIRE(entry.update(press(false, false, true), 1) == NameEntry::Event::LetterRemoved);
    REQUIRE(entry.name() == "B");
    REQUIRE(entry.pendingLetter() == '9');
    REQUIRE(entry.update(MenuInput{}, 1) == NameEntry::Event::None);
    REQUIRE(entry.editing());
}

TEST_CASE("the end mark accepts the name and it flashes before finishing", "[game][menu][name]") {
    NameEntry entry;
    entry.begin("JOE");
    REQUIRE(entry.name() == "JOE");
    // Right on the end mark only skips; Select accepts.
    REQUIRE(entry.update(press(false, false, false, true), 1) == NameEntry::Event::None);
    REQUIRE(entry.editing());
    REQUIRE(entry.update(press(false, false, false, false, true), 1) == NameEntry::Event::Accepted);
    REQUIRE(entry.flashing());
    REQUIRE(entry.name() == "JOE");
    entry.update(MenuInput{}, NameEntry::kFlashTicks);
    REQUIRE(entry.flashing());
    entry.update(MenuInput{}, 1);
    REQUIRE(entry.finished());
    REQUIRE(entry.update(press(true), 1) == NameEntry::Event::None);
}

TEST_CASE("an empty accepted name is replaced and a full one completes itself",
          "[game][menu][name]") {
    NameEntry entry;
    entry.begin("");
    REQUIRE(entry.update(press(false, false, false, false, true), 1) == NameEntry::Event::Accepted);
    REQUIRE_FALSE(entry.name().empty());
    REQUIRE(entry.name().size() <= NameEntry::kMaxLength);

    NameEntry full;
    full.begin("ABCDEF");
    REQUIRE(full.name() == "ABCDE");
    REQUIRE(full.pendingLetter() == 'F');
    REQUIRE(full.update(press(false, false, false, true), 1) == NameEntry::Event::Accepted);
    REQUIRE(full.name() == "ABCDEF");
    REQUIRE(full.flashing());
}

TEST_CASE("typed letters go straight into the name", "[game][menu][name]") {
    REQUIRE(NameEntry::typedLetter('a') == 'A');
    REQUIRE(NameEntry::typedLetter('Z') == 'Z');
    REQUIRE(NameEntry::typedLetter('7') == '7');
    REQUIRE(NameEntry::typedLetter(' ') == '_');
    REQUIRE_FALSE(NameEntry::typedLetter('!').has_value());

    NameEntry entry;
    entry.begin("");
    MenuInput typed;
    typed.typed = "bo!b ";
    REQUIRE(entry.update(typed, 1) == NameEntry::Event::LetterAdded);
    REQUIRE(entry.name() == "BOB_");
    REQUIRE(entry.pendingLetter() == NameEntry::kEndMark);

    MenuInput erase;
    erase.erase = true;
    REQUIRE(entry.update(erase, 1) == NameEntry::Event::LetterRemoved);
    REQUIRE(entry.name() == "BOB");
    REQUIRE(entry.pendingLetter() == '_');

    // Erasing an empty name is nothing to the picker; the lane backs out instead.
    NameEntry empty;
    empty.begin("");
    REQUIRE(empty.update(erase, 1) == NameEntry::Event::None);
    REQUIRE(empty.editing());

    // Enter takes a typed name; a sixth letter completes it by itself.
    typed.typed = "y";
    REQUIRE(entry.update(typed, 1) == NameEntry::Event::LetterAdded);
    REQUIRE(entry.update(press(false, false, false, false, true), 1) == NameEntry::Event::Accepted);
    REQUIRE(entry.name() == "BOBY");
    REQUIRE(entry.flashing());
    NameEntry full;
    full.begin("GORDO");
    typed.typed = "n";
    REQUIRE(full.update(typed, 1) == NameEntry::Event::Accepted);
    REQUIRE(full.name() == "GORDON");
}

TEST_CASE("a held direction keeps cycling letters, faster and faster", "[game][menu][name]") {
    NameEntry entry;
    entry.begin("");
    MenuInput hold;
    hold.up = true;
    hold.upHeld = true;
    REQUIRE(entry.update(hold, 1) == NameEntry::Event::LetterChanged);
    REQUIRE(entry.pendingLetter() == 'A');
    hold.up = false; // still held, no new press
    REQUIRE(entry.update(hold, NameEntry::kRepeatLadder[0] - 1) == NameEntry::Event::None);
    REQUIRE(entry.update(hold, 1) == NameEntry::Event::LetterChanged);
    REQUIRE(entry.pendingLetter() == 'B');
    REQUIRE(entry.update(hold, NameEntry::kRepeatLadder[1] - 1) == NameEntry::Event::None);
    REQUIRE(entry.update(hold, 1) == NameEntry::Event::LetterChanged);
    REQUIRE(entry.pendingLetter() == 'C');
    // Down the ladder the repeats come every tick.
    for (std::size_t step = 2; step < NameEntry::kRepeatLadder.size(); ++step) {
        entry.update(hold, NameEntry::kRepeatLadder[step]);
    }
    const char before = entry.pendingLetter();
    REQUIRE(entry.update(hold, 1) == NameEntry::Event::LetterChanged);
    REQUIRE(entry.pendingLetter() == NameEntry::nextLetter(before));

    // Letting go stops the repeats; holding the other way starts over from the long delay.
    REQUIRE(entry.update(MenuInput{}, 10) == NameEntry::Event::None);
    MenuInput down;
    down.downHeld = true;
    REQUIRE(entry.update(down, 1) == NameEntry::Event::None); // the hold is noticed
    REQUIRE(entry.update(down, NameEntry::kRepeatLadder[0] - 1) == NameEntry::Event::None);
    const char held = entry.pendingLetter();
    REQUIRE(entry.update(down, 1) == NameEntry::Event::LetterChanged);
    REQUIRE(entry.pendingLetter() == NameEntry::previousLetter(held));
    MenuInput both;
    both.upHeld = true;
    both.downHeld = true;
    REQUIRE(entry.update(both, 100) == NameEntry::Event::None);
}

} // namespace
