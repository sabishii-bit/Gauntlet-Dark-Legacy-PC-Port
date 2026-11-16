#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"

#include "FakeDarkLegacyArt.h"
#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
#include "LogCapture.h"
#include "game/config/GameConfig.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyleSlot.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/players/PlayerControls.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using test::FakeHudPart;

/** The seat whose presses a browse carries. */
constexpr s32 kBrowsingSeat = 1;

/** A catalog offering the looks `fake` and `other`, and `gone`, whose files have gone so that it
 * does not open; the party they open for; the frame and view they are drawn with. */
struct SlotFixture {
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    test::FakeHudStyles styles;
    Canvas canvas;
    HudFrame frame{.canvas = canvas, .device = device, .darkLegacy = art};
    const HudView view{};
    const std::array<HudMember, 2> party{HudMember{.seat = 1, .classIndex = 3, .color = 2},
                                         HudMember{.seat = 3, .classIndex = 0, .color = 1}};

    SlotFixture() {
        styles.offered = {{.id = "fake", .label = "hud.style.fake", .usable = true},
                          {.id = "other", .label = "hud.style.other", .usable = true},
                          {.id = "gone", .label = "hud.style.gone", .usable = false}};
        styles.failing = {"gone"};
    }

    void open(HudStyleSlot& slot, std::string_view id) {
        slot.open(&styles, id, device, art, party);
    }
    void settle(HudStyleSlot& slot, std::string_view id) { slot.settle(id, device, art, party); }
    /** Runs the call `part` of the slot's look, as the scene does in a frame or a step. */
    void call(HudStyleSlot& slot, FakeHudPart part) {
        slot.run(art, [&](HudPainter& look) {
            switch (part) {
            case FakeHudPart::Prepare: look.prepare(frame, view); break;
            case FakeHudPart::Status: look.drawStatus(frame, view); break;
            case FakeHudPart::RuneMeter: look.drawRuneMeter(frame, view); break;
            case FakeHudPart::Hourglass: look.drawHourglass(frame, view); break;
            case FakeHudPart::BossMeters: look.drawBossMeters(frame, view); break;
            case FakeHudPart::Selectors: look.drawSelectors(frame, view); break;
            case FakeHudPart::Help: look.drawHelp(frame, view); break;
            case FakeHudPart::Browse:
                look.browse(kBrowsingSeat, SelectorInput{.down = true});
                break;
            }
        });
    }
    std::vector<HudMember> members() const { return {party.begin(), party.end()}; }
};

} // namespace

TEST_CASE("the slot draws the chosen look, and Dark Legacy's for its own id, an id no offer "
          "names, or a look that fails",
          "[game][hud][hud-style-slot]") {
    SlotFixture fixture;
    HudStyleSlot slot;

    // The look chosen opens for the party, with Dark Legacy's art to borrow from, and draws.
    fixture.styles.borrowed = "SM_KEY_BLU";
    fixture.open(slot, "fake");
    CHECK(slot.shown() == "fake");
    CHECK(slot.painter(fixture.art).id() == "fake");
    CHECK(&slot.painter(fixture.art) != &fixture.art.look());
    REQUIRE(fixture.styles.opened == std::vector<std::string>{"fake"});
    CHECK(fixture.styles.parties.front() == fixture.members());
    CHECK(fixture.art.lookups ==
          std::vector{test::DarkLegacyLookup{.set = test::DarkLegacyLookup::Set::Static,
                                             .name = "SM_KEY_BLU"}});
    fixture.call(slot, FakeHudPart::Status);
    CHECK(fixture.styles.record.count("fake", FakeHudPart::Status) == 1);
    CHECK(fixture.art.record.calls.empty());

    // Dark Legacy's own id opens nothing and asks the catalog nothing, so a scene in the base
    // game's look never looks for an expansion's files; opening it lets the look held go.
    const u32 asked = fixture.styles.asks;
    CHECK(test::warningsWhile([&] { fixture.open(slot, kDarkLegacyHudStyle); }).empty());
    CHECK(fixture.styles.asks == asked);
    CHECK(slot.shown() == kDarkLegacyHudStyle);
    CHECK(&slot.painter(fixture.art) == &fixture.art.look());
    CHECK(fixture.styles.record.destroyed() == std::vector<std::string>{"fake"});
    // An id no expansion offers is looked for among the offers, and opens nothing without a word.
    CHECK(test::warningsWhile([&] { fixture.open(slot, "future"); }).empty());
    CHECK(fixture.styles.asks == asked + 1);
    CHECK(slot.shown() == kDarkLegacyHudStyle);
    CHECK(&slot.painter(fixture.art) == &fixture.art.look());
    CHECK(fixture.styles.opened == std::vector<std::string>{"fake"});

    // A look offered that cannot be drawn is still asked for, so that its opener says why; the
    // slot adds no word of its own, and Dark Legacy's draws in its place.
    CHECK(test::warningsWhile([&] { fixture.open(slot, "gone"); }).empty());
    CHECK(fixture.styles.opened == std::vector<std::string>{"fake", "gone"});
    CHECK(slot.shown() == kDarkLegacyHudStyle);
    fixture.call(slot, FakeHudPart::Status);
    CHECK(fixture.art.record.count(kDarkLegacyHudStyle, FakeHudPart::Status) == 1);

    // Without a catalog every id is Dark Legacy's.
    slot.open(nullptr, "fake", fixture.device, fixture.art, fixture.party);
    CHECK(slot.shown() == kDarkLegacyHudStyle);
    CHECK(fixture.styles.opened.size() == 2);
}

TEST_CASE("a look that fails to open is not asked for again until the id changes",
          "[game][hud][hud-style-slot]") {
    SlotFixture fixture;
    HudStyleSlot slot;
    fixture.open(slot, "gone");
    // The steps after say nothing either: the opener said why, once.
    const std::vector<std::string> warnings = test::warningsWhile([&] {
        for (s32 step = 0; step < 3; ++step) {
            CHECK_FALSE(slot.unsettled("gone"));
            fixture.settle(slot, "gone");
        }
    });
    CHECK(warnings.empty());
    CHECK(fixture.styles.opened == std::vector<std::string>{"gone"});
    CHECK(slot.shown() == kDarkLegacyHudStyle);

    // Another choice is asked for at the next step, and the failing one again only once it is
    // chosen anew.
    REQUIRE(slot.unsettled("fake"));
    fixture.settle(slot, "fake");
    CHECK(slot.shown() == "fake");
    fixture.settle(slot, "gone");
    CHECK(fixture.styles.opened == std::vector<std::string>{"gone", "fake", "gone"});
    CHECK(slot.shown() == kDarkLegacyHudStyle);
    CHECK(fixture.styles.record.destroyed() == std::vector<std::string>{"fake"});
}

TEST_CASE("a changed id waits for a settle; opening asks for nothing",
          "[game][hud][hud-style-slot]") {
    SlotFixture fixture;
    HudStyleSlot slot;
    fixture.open(slot, "fake");
    // A scene's opening leaves its first step nothing to settle.
    CHECK_FALSE(slot.unsettled("fake"));

    // Another choice leaves the look drawn as it is until a step settles the slot.
    CHECK(slot.unsettled("other"));
    fixture.call(slot, FakeHudPart::Status);
    CHECK(slot.unsettled("other"));
    CHECK(slot.shown() == "fake");
    CHECK(fixture.styles.record.count("fake", FakeHudPart::Status) == 1);
    CHECK(fixture.styles.record.destroyed().empty());
    CHECK(fixture.styles.opened == std::vector<std::string>{"fake"});

    // The step lets the old look go first, so that its art has gone before the new look takes
    // its own, then opens the new one for the party it is given.
    const std::array joined{HudMember{.seat = 0, .classIndex = 5, .color = 3}};
    slot.settle("other", fixture.device, fixture.art, joined);
    CHECK(slot.shown() == "other");
    CHECK(fixture.styles.record.destroyed() == std::vector<std::string>{"fake"});
    CHECK(fixture.styles.record.goneWhenMade == std::vector<u32>{0, 1});
    CHECK(fixture.styles.parties.back() == std::vector<HudMember>{joined.front()});
    CHECK_FALSE(slot.unsettled("other"));

    // Back to Dark Legacy's look, which opens nothing and asks the catalog nothing.
    REQUIRE(slot.unsettled(kDarkLegacyHudStyle));
    const u32 asked = fixture.styles.asks;
    fixture.settle(slot, kDarkLegacyHudStyle);
    CHECK(fixture.styles.asks == asked);
    CHECK(slot.shown() == kDarkLegacyHudStyle);
    CHECK(fixture.styles.record.destroyed() == std::vector<std::string>{"fake", "other"});
    CHECK(fixture.styles.opened == std::vector<std::string>{"fake", "other"});
    CHECK_FALSE(slot.unsettled(kDarkLegacyHudStyle));
}

TEST_CASE("a look that throws in play is retired for the scene, and Dark Legacy draws on",
          "[game][hud][hud-style-slot]") {
    const FakeHudPart part = GENERATE(
        FakeHudPart::Prepare, FakeHudPart::Status, FakeHudPart::RuneMeter, FakeHudPart::Hourglass,
        FakeHudPart::BossMeters, FakeHudPart::Selectors, FakeHudPart::Help, FakeHudPart::Browse);
    CAPTURE(static_cast<s32>(part));
    SlotFixture fixture;
    fixture.styles.throwsIn = part;
    HudStyleSlot slot;
    fixture.open(slot, "fake");
    REQUIRE(slot.shown() == "fake");

    // Whatever call it throws in, the slot catches it and says so in one line.
    std::vector<std::string> warnings = test::warningsWhile([&] { fixture.call(slot, part); });
    CHECK(warnings == std::vector<std::string>{"HUD: the fake look failed (the fake look broke); "
                                               "drawing Dark Legacy's for the rest of the scene"});
    CHECK(fixture.styles.record.calls ==
          std::vector{test::FakeHudCall{.look = "fake",
                                        .part = part,
                                        .seat = part == FakeHudPart::Browse ? kBrowsingSeat : -1}});
    CHECK(slot.shown() == kDarkLegacyHudStyle);
    CHECK(&slot.painter(fixture.art) == &fixture.art.look());
    // It is kept until a step lets it go, since the frames in flight may still draw with it.
    CHECK(fixture.styles.record.destroyed().empty());
    CHECK(slot.unsettled("fake"));

    // Dark Legacy's look takes every call after it, without another word.
    warnings = test::warningsWhile([&] {
        fixture.call(slot, FakeHudPart::Status);
        fixture.call(slot, FakeHudPart::Help);
    });
    CHECK(warnings.empty());
    CHECK(fixture.art.record.count(kDarkLegacyHudStyle, FakeHudPart::Status) == 1);
    CHECK(fixture.art.record.count(kDarkLegacyHudStyle, FakeHudPart::Help) == 1);
    CHECK(fixture.styles.record.calls.size() == 1);

    // The next step lets it go and does not open it again while the choice stands.
    fixture.settle(slot, "fake");
    CHECK(fixture.styles.record.destroyed() == std::vector<std::string>{"fake"});
    CHECK_FALSE(slot.unsettled("fake"));
    CHECK(fixture.styles.opened == std::vector<std::string>{"fake"});
    CHECK(slot.shown() == kDarkLegacyHudStyle);

    // What Dark Legacy's own look throws goes on: it is the game's, and is never retired.
    fixture.art.darkLegacyLook.throwsIn = FakeHudPart::Status;
    CHECK_THROWS_AS(fixture.call(slot, FakeHudPart::Status), std::runtime_error);
}

TEST_CASE("closing the slot lets its painters go", "[game][hud][hud-style-slot]") {
    SlotFixture fixture;
    HudStyleSlot slot;
    // The look drawn goes as the slot closes.
    fixture.open(slot, "fake");
    slot.close();
    CHECK(fixture.styles.record.destroyed() == std::vector<std::string>{"fake"});
    CHECK(slot.shown() == kDarkLegacyHudStyle);

    // So does one retired in play that no step has let go yet.
    fixture.styles.throwsIn = FakeHudPart::Status;
    fixture.open(slot, "other");
    CHECK(test::warningsWhile([&] { fixture.call(slot, FakeHudPart::Status); }).size() == 1);
    REQUIRE(fixture.styles.record.destroyed().size() == 1);
    slot.close();
    CHECK(fixture.styles.record.destroyed() == std::vector<std::string>{"fake", "other"});

    // A closed slot holds no catalog either: settling it opens nothing.
    fixture.settle(slot, "fake");
    CHECK(slot.shown() == kDarkLegacyHudStyle);
    CHECK(fixture.styles.opened == std::vector<std::string>{"fake", "other"});
}
