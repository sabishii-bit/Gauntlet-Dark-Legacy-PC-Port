#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/world/WorldCamera.h"

#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
#include "HudLookScene.h"
#include "LogCapture.h"
#include "TestSupport.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Inventory.h"
#include "game/characters/Party.h"
#include "game/config/GameConfig.h"
#include "game/hud/CompanionArrows.h"
#include "game/hud/DarkLegacyHudPainter.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/hud/PickupHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/StatusBox.h"
#include "game/level/ExitPortals.h"
#include "game/level/LevelCatalog.h"
#include "game/party/PartyCombo.h"
#include "game/party/PlayInput.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/ComboMove.h"
#include "game/players/PlayerControls.h"
#include "game/players/TurboMeter.h"
#include "game/rollback/ConsoleAction.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using test::FakeHudPart;
using test::HudPresses;
using test::Press;
using test::pressed;

constexpr std::string_view kFakeLook = "fake";
constexpr std::string_view kOtherLook = "other";
/** Where a party stands to be greeted by Sumner and handed his scroll. */
constexpr Vec3 kSumnerSpot{3.3f, 2.1f, -49.0f};
/** How far behind the eye a companion is stood, for an arrow to point to it down the view. */
constexpr f32 kBehindEye = 4.0f;

/** Every seat's selector pressed one way, and nothing else. */
PlayScene::Inputs everyone(Press way) {
    PlayScene::Inputs inputs{};
    for (PlayInput& input : inputs) {
        input.selector = pressed(way);
    }
    return inputs;
}

/** A level opened as the native digest tests open theirs, its HUD drawn in a look of the fake
 * catalog, which offers `fake` and `other`, or in Dark Legacy's alone. Skips without the game
 * data. */
struct HudScene : test::HudLookScene<test::FakeHudStyles> {
    explicit HudScene(std::string_view stage, s32 seats = 2) : HudLookScene(stage, seats) {
        styles.offered = {
            {.id = std::string{kFakeLook}, .label = "hud.style.fake", .usable = true},
            {.id = std::string{kOtherLook}, .label = "hud.style.other", .usable = true}};
        styles.idleWaits = &device.idleWaits;
    }
};

/** The seats a look was asked to browse, or to steer by an open selector's presses, from call
 * `from` of `record` on, in turn. */
std::vector<s32> browsedFrom(const test::FakeHudRecord& record, usize from,
                             FakeHudPart part = FakeHudPart::Browse) {
    std::vector<s32> seats;
    for (usize call = from; call < record.calls.size(); ++call) {
        if (record.calls[call].part == part) {
            seats.push_back(record.calls[call].seat);
        }
    }
    return seats;
}

/** Which of the looks `record` saw made went last; there must be one gone. */
usize lastGone(const test::FakeHudRecord& record) {
    REQUIRE(record.departures > 0);
    return static_cast<usize>(std::ranges::find(record.letGo, record.departures) -
                              record.letGo.begin());
}

/** The vertices of `draws`, in the order they were drawn. */
std::vector<ImmediateVertex> verticesOf(std::span<const test::RecordedDraw> draws) {
    std::vector<ImmediateVertex> vertices;
    for (const test::RecordedDraw& draw : draws) {
        vertices.insert(vertices.end(), draw.vertices.begin(), draw.vertices.end());
    }
    return vertices;
}

/** Whether `drawn` holds the run `part`, unbroken. */
bool holds(const std::vector<ImmediateVertex>& drawn, const std::vector<ImmediateVertex>& part) {
    return std::ranges::search(drawn, part).begin() != drawn.end();
}

} // namespace

TEST_CASE("a HUD look's painter and its pages change no byte of the scene's digests",
          "[play-scene][hud-style][native-state-digest][assets]") {
    const s32 seats = GENERATE(2, 4);
    CAPTURE(seats);
    auto darkLegacy = std::make_unique<HudScene>("G2", seats);
    auto paged = std::make_unique<HudScene>("G2", seats);
    for (HudScene* fixture : {darkLegacy.get(), paged.get()}) {
        fixture->party[0].save.progress().inventory.powerups[0] = test::kSelectorPowerup;
    }
    // The look borrows a picture of Dark Legacy's boxes as it opens, which uploads it then.
    paged->styles.borrowed = "SM_KEY_BLU";
    darkLegacy->open();
    paged->open(kFakeLook);
    REQUIRE(paged->styles.opened == std::vector{std::string{kFakeLook}});
    CHECK(paged->device.texturesCreated == darkLegacy->device.texturesCreated + 1);
    CHECK(darkLegacy->digest() == paged->digest());

    const PowerupSelector& first = paged->scene.selector(0);
    for (const u32 anchor : {HudPresses::kFirstAnchor, HudPresses::kLastTick}) {
        while (paged->tick < anchor) {
            const PlayScene::Inputs inputs = test::hudInputsAt(paged->tick + 1);
            darkLegacy->step(inputs);
            paged->step(inputs);
            // Both draw every tick: the one Dark Legacy's boxes, the other the look.
            darkLegacy->render(0.5f);
            paged->render(0.5f);
            CAPTURE(paged->tick);
            REQUIRE(darkLegacy->digest() == paged->digest());
            // The first seat's selector stands open from its up to its down.
            if (paged->tick == HudPresses::kOpenTick + HudPresses::kEvery) {
                CHECK(first.state() != PowerupSelector::State::Closed);
            } else if (paged->tick == HudPresses::kCloseTick + 2 * HudPresses::kEvery) {
                CHECK(first.state() == PowerupSelector::State::Closed);
            }
        }
        PlayScene::LocalSnapshot plain;
        PlayScene::LocalSnapshot looked;
        REQUIRE(darkLegacy->scene.saveLocal(plain));
        REQUIRE(paged->scene.saveLocal(looked));
        CHECK(darkLegacy->digest(&plain) == paged->digest(&looked));
    }
    // The look prepared every frame; the closed selectors' presses turned its pages, and the
    // first seat's right, while its selector stood open, steered it.
    const test::FakeHudRecord& record = paged->styles.record;
    CHECK(record.count(kFakeLook, FakeHudPart::Prepare) == HudPresses::kLastTick);
    CHECK(record.count(kFakeLook, FakeHudPart::Browse) > 0);
    CHECK(browsedFrom(record, 0, FakeHudPart::Steer) == std::vector{0});
    REQUIRE(record.pages.size() == 1);
    CHECK(record.pages.front() != test::FakeHudPages{});
}

TEST_CASE("switching the HUD look mid-level changes no byte of the digests and swaps only at a "
          "step, after the device idles",
          "[play-scene][hud-style][native-state-digest][assets]") {
    auto darkLegacy = std::make_unique<HudScene>("G2");
    auto switching = std::make_unique<HudScene>("G2");
    // The Dark Legacy scene has the catalog the game links too, which its look never asks.
    darkLegacy->open({}, true);
    switching->open(kFakeLook);
    const test::FakeHudRecord& record = switching->styles.record;
    // The scene's opening opens its look at once, waiting for nothing.
    REQUIRE(switching->styles.opened == std::vector{std::string{kFakeLook}});
    CHECK(switching->device.idleWaits == 0);

    // Every kSwitchEvery ticks the settings choose the next of kChoices, as the pause menu
    // saves a choice while the scene stands paused under it.
    constexpr u32 kSwitchEvery = 50;
    constexpr std::array kChoices{kOtherLook, kDarkLegacyHudStyle, kFakeLook};
    std::string_view shown = kFakeLook;
    u32 switches = 0;
    while (switching->tick < HudPresses::kLastTick) {
        const u32 next = switching->tick + 1;
        const PlayScene::Inputs inputs = test::hudInputsAt(next);
        if (next % kSwitchEvery == 0) {
            const std::string_view chosen =
                kChoices.at((next / kSwitchEvery - 1) % kChoices.size());
            CAPTURE(next, shown, chosen);
            switching->config.display.hudStyle = std::string{chosen};
            const usize opens = switching->styles.opened.size();
            const usize calls = record.calls.size();
            const u32 waits = switching->device.idleWaits;
            const u32 gone = record.departures;
            const u32 asked = switching->styles.asks;
            // Under the settings the scene draws on in the look it had, and opens and lets go of
            // nothing.
            switching->render(-1.0f, true);
            CHECK(switching->styles.opened.size() == opens);
            CHECK(switching->device.idleWaits == waits);
            CHECK(record.departures == gone);
            if (shown == kDarkLegacyHudStyle) {
                CHECK(record.calls.size() == calls);
            } else {
                REQUIRE(record.calls.size() > calls);
                CHECK(record.calls.back().look == shown);
            }
            // The next step waits for the device, then lets the old look go and opens the new.
            darkLegacy->step(inputs);
            switching->step(inputs);
            ++switches;
            CHECK(switching->device.idleWaits == waits + 1);
            if (shown == kDarkLegacyHudStyle) {
                CHECK(record.departures == gone);
            } else {
                REQUIRE(record.departures == gone + 1);
                const usize went = lastGone(record);
                CHECK(record.made.at(went) == shown);
                CHECK(record.idleWaitsAtLetGo.at(went) > waits);
            }
            if (chosen == kDarkLegacyHudStyle) {
                // Dark Legacy's look is the base game's: no expansion's files are looked for.
                CHECK(switching->styles.opened.size() == opens);
                CHECK(switching->styles.asks == asked);
            } else {
                REQUIRE(switching->styles.opened.size() == opens + 1);
                CHECK(switching->styles.opened.back() == chosen);
            }
            shown = chosen;
        } else {
            darkLegacy->step(inputs);
            switching->step(inputs);
        }
        darkLegacy->render(0.5f);
        switching->render(0.5f);
        CAPTURE(switching->tick);
        REQUIRE(darkLegacy->digest() == switching->digest());
    }
    // Only the switches waited for the device, and the scene in Dark Legacy's look never asked
    // its catalog for a look as it opened or as it stepped.
    CHECK(switches == HudPresses::kLastTick / kSwitchEvery);
    CHECK(switching->device.idleWaits == switches);
    CHECK(darkLegacy->device.idleWaits == 0);
    CHECK(darkLegacy->styles.asks == 0);
    CHECK(darkLegacy->styles.opened.empty());
}

TEST_CASE("a closed selector's down, left and right reach the look's pages, an open one's left "
          "and right its arrows, and nothing else",
          "[play-scene][hud-style][assets]") {
    // Three players: the first and the third carry a powerup their selectors can name, and the
    // third plays on another machine.
    constexpr s32 kSeats = 3;
    constexpr u8 kLocalSeats = 0b011;
    auto darkLegacy = std::make_unique<HudScene>("G2", kSeats);
    auto paged = std::make_unique<HudScene>("G2", kSeats);
    for (HudScene* fixture : {darkLegacy.get(), paged.get()}) {
        fixture->party[0].save.progress().inventory.powerups[0] = test::kSelectorPowerup;
        fixture->party[2].save.progress().inventory.powerups[0] = test::kSelectorPowerup;
    }
    darkLegacy->open();
    paged->open(kFakeLook);
    for (HudScene* fixture : {darkLegacy.get(), paged.get()}) {
        fixture->scene.setLocalSeats(kLocalSeats);
        fixture->settle();
    }
    REQUIRE(darkLegacy->tick == paged->tick);
    const test::FakeHudRecord& record = paged->styles.record;
    usize from = record.calls.size();
    std::vector<s32> steered; ///< the seats the last press steered the look by
    // Both scenes step on the same presses and stay alike; the seats browsed meanwhile, and
    // those steered.
    const auto press = [&](const PlayScene::Inputs& inputs) {
        darkLegacy->step(inputs);
        paged->step(inputs);
        CHECK(darkLegacy->digest() == paged->digest());
        std::vector<s32> seats = browsedFrom(record, from);
        steered = browsedFrom(record, from, FakeHudPart::Steer);
        from = record.calls.size();
        return seats;
    };
    const PowerupSelector& first = paged->scene.selector(0);
    const PowerupSelector& third = paged->scene.selector(2);
    const auto slide = [&](PowerupSelector::State until) {
        for (s32 tick = 0; tick < 2 * PowerupSelector::kSlide && first.state() != until; ++tick) {
            CHECK(press({}).empty());
            CHECK(steered.empty());
        }
        REQUIRE(first.state() == until);
    };

    // On closed selectors down, left and right turn this machine's seats' pages, never the
    // third seat's, and steer nothing.
    for (const Press way : {Press::Down, Press::Left, Press::Right}) {
        CAPTURE(static_cast<s32>(way));
        CHECK(press(everyone(way)) == std::vector{0, 1});
        CHECK(steered.empty());
    }
    // Up turns none, opening the first and third seats' selectors; nor does a menu's own down.
    CHECK(press(everyone(Press::Up)).empty());
    REQUIRE(first.state() == PowerupSelector::State::SlidingIn);
    REQUIRE(third.state() == PowerupSelector::State::SlidingIn);
    PlayScene::Inputs menuDown{};
    for (PlayInput& input : menuDown) {
        input.menu.down = true;
    }
    CHECK(press(menuDown).empty());
    // While the first seat's selector slides in, stands open and slides away, its presses are
    // the selector's, and the second seat's alone turn a page. Standing open, it moves by left
    // and right, of which the look is told, even with nothing else to come up; the third
    // seat's, another machine's, never.
    CHECK(press(everyone(Press::Down)) == std::vector{1});
    CHECK(steered.empty());
    slide(PowerupSelector::State::Open);
    REQUIRE(third.state() == PowerupSelector::State::Open);
    CHECK(press(everyone(Press::Right)) == std::vector{1});
    CHECK(steered == std::vector{0});
    CHECK(press(everyone(Press::Left)) == std::vector{1});
    CHECK(steered == std::vector{0});
    CHECK(press(everyone(Press::Down)) == std::vector{1});
    CHECK(steered.empty());
    REQUIRE(first.state() == PowerupSelector::State::SlidingOut);
    CHECK(press(everyone(Press::Left)) == std::vector{1});
    CHECK(steered.empty());
    slide(PowerupSelector::State::Closed);
    // Closed again, its presses turn its pages once more.
    CHECK(press(everyone(Press::Left)) == std::vector{0, 1});
    CHECK(steered.empty());
}

TEST_CASE("the selector's presses reach no page while the scene holds them",
          "[play-scene][hud-style][assets]") {
    auto fixture = std::make_unique<HudScene>("L1", 1);
    PlayScene& scene = fixture->scene;
    const test::FakeHudRecord& record = fixture->styles.record;
    const auto browses = [&] { return record.count(kFakeLook, FakeHudPart::Browse); };
    // Steps with the one player's selector pressed down, left and right in turn, over `inputs`.
    constexpr std::array kTurns{Press::Down, Press::Left, Press::Right};
    usize turn = 0;
    const auto press = [&](PlayScene::Inputs inputs = {}) {
        inputs[0].selector = pressed(kTurns.at(turn++ % kTurns.size()));
        fixture->step(inputs);
    };

    // Through the entrance nothing is browsed; once the party has come in, its presses are.
    fixture->open(kFakeLook);
    REQUIRE(scene.spawning());
    for (u32 guard = 0; guard < 600 && fixture->entering(); ++guard) {
        press();
    }
    REQUIRE_FALSE(fixture->entering());
    CHECK(browses() == 0);
    press();
    CHECK(browses() == 1);

    // While Sumner's hint menu is up nothing is browsed, nor while it burns away; then the
    // presses are again.
    REQUIRE(scene.relocate(kSumnerSpot));
    for (u32 guard = 0; guard < 600 && !scene.hints().active(); ++guard) {
        fixture->step({});
    }
    REQUIRE(scene.hints().active());
    usize before = browses();
    for (s32 tick = 0; tick < 30; ++tick) {
        press();
    }
    PlayScene::Inputs back{};
    back[0].menu.back = true;
    for (u32 guard = 0; guard < 600 && scene.hints().active(); ++guard) {
        press(guard % 2 == 0 ? back : PlayScene::Inputs{});
    }
    REQUIRE_FALSE(scene.hints().active());
    CHECK(browses() == before);
    press();
    CHECK(browses() == before + 1);

    // A new party's welcome: nothing is browsed through the entrance and Sumner's scroll, nor
    // through the cut to the crystals after it; then the presses are again. Some ticks of the
    // scroll and of the cut come once the party has come in, when that hold alone stands.
    scene.close();
    fixture->options.welcome = true;
    fixture->open(kFakeLook);
    before = browses();
    PlayScene::Inputs accept{};
    accept[0].menu.select = true;
    u32 scrollHeld = 0;
    for (u32 guard = 0; guard < 2000 && scene.intro() != PlayScene::Intro::Crystal; ++guard) {
        scrollHeld += scene.scroll().active() && !fixture->entering() ? 1 : 0;
        press(scene.scroll().active() && guard % 20 == 19 ? accept : PlayScene::Inputs{});
    }
    REQUIRE(scene.intro() == PlayScene::Intro::Crystal);
    CHECK(scrollHeld > 0);
    CHECK(browses() == before);
    u32 cutHeld = 0;
    for (u32 guard = 0; guard < 600 && scene.intro() == PlayScene::Intro::Crystal; ++guard) {
        cutHeld += !scene.scroll().active() && !fixture->entering() ? 1 : 0;
        press();
    }
    REQUIRE(scene.intro() == PlayScene::Intro::Done);
    CHECK(cutHeld > 0);
    CHECK(browses() == before);
    press();
    CHECK(browses() == before + 1);

    // Nothing is browsed while the party goes through a portal, from the moment it has gone
    // until it travels.
    const ExitPortals& exits = scene.portals();
    usize g1 = 0;
    while (g1 < exits.size() &&
           (!exits.portal(g1).destination || exits.portal(g1).destination->name != "G1")) {
        ++g1;
    }
    REQUIRE(g1 < exits.size());
    REQUIRE(scene.actor(0) != nullptr);
    REQUIRE(
        scene.noclipMove(0, exits.portal(g1).position - scene.actor(0)->position(), std::nullopt));
    for (u32 guard = 0; guard < 600 && !scene.leaving(); ++guard) {
        fixture->step({});
    }
    REQUIRE(scene.leaving());
    before = browses();
    PlayOutcome outcome = PlayOutcome::Running;
    u32 leavingHeld = 0;
    for (; leavingHeld < 600 && outcome == PlayOutcome::Running; ++leavingHeld) {
        PlayScene::Inputs inputs{};
        inputs[0].selector = pressed(kTurns.at(turn++ % kTurns.size()));
        outcome = scene.update(HudScene::kStep, inputs);
    }
    CHECK(outcome == PlayOutcome::Travel);
    CHECK(leavingHeld > 1);
    CHECK(browses() == before);

    // Nor while the game is being quit.
    scene.close();
    fixture->options.welcome = false;
    fixture->open(kFakeLook);
    fixture->settle();
    before = browses();
    press();
    CHECK(browses() == before + 1);
    scene.startGameOver();
    for (s32 tick = 0; tick < 60; ++tick) {
        press();
    }
    CHECK(scene.gameOver().active());
    CHECK(browses() == before + 1);

    // Nor once the console has sent the party elsewhere, while the journey waits on the game.
    scene.close();
    fixture->open(kFakeLook);
    fixture->settle();
    const std::array travel{ConsoleAction{.seat = 0, .line = "goto G1"}};
    REQUIRE(scene.step(HudScene::kStep, {}, travel).outcome == PlayOutcome::Travel);
    REQUIRE(scene.consoleTravelRequest().has_value());
    before = browses();
    for (s32 tick = 0; tick < 10; ++tick) {
        PlayScene::Inputs inputs{};
        inputs[0].selector = pressed(kTurns.at(turn++ % kTurns.size()));
        CHECK(scene.step(HudScene::kStep, inputs, {}).outcome == PlayOutcome::Travel);
    }
    CHECK(browses() == before);
}

TEST_CASE("a seat's presses reach no page while its member cannot act",
          "[play-scene][hud-style][assets]") {
    // A warrior and, in the second seat, a wizard, whose combo lifts its partner and sets it
    // down again; both meters full, so that the wizard may take hold of the warrior, and each
    // member of a class other than its colour.
    auto fixture = std::make_unique<HudScene>("G2");
    fixture->party[0].save.color = 2;
    fixture->party[1].save.character = kWizardClass;
    for (PartyMember& member : fixture->party) {
        member.turbo = TurboMeter::kFull;
    }
    fixture->open(kFakeLook);
    fixture->settle();
    PlayScene& scene = fixture->scene;
    const test::FakeHudRecord& record = fixture->styles.record;
    usize from = record.calls.size();
    // Steps with both seats' selectors pressed `way`, over `inputs`; the seats browsed.
    const auto press = [&](Press way, PlayScene::Inputs inputs = {}) {
        for (PlayInput& input : inputs) {
            input.selector = pressed(way);
        }
        fixture->step(inputs);
        std::vector<s32> seats = browsedFrom(record, from);
        from = record.calls.size();
        return seats;
    };
    REQUIRE(press(Press::Down) == std::vector{0, 1});

    // A member flying free under the console's noclip heeds no selector until it lands.
    scene.setCheats(0, ConsoleCheats{.noclip = true});
    CHECK(press(Press::Left) == std::vector{1});
    scene.setCheats(0, ConsoleCheats{});
    CHECK(press(Press::Right) == std::vector{0, 1});

    // Nor does one its partner's combo has taken up, while the partner holding it does.
    REQUIRE(scene.actor(0) != nullptr);
    REQUIRE(scene.relocate(scene.actor(0)->position()));
    const Vec3 across = scene.actor(0)->position() - scene.actor(1)->position();
    REQUIRE(glm::length(Vec2{across.x, across.z}) < ComboMove::kReach);
    REQUIRE(std::abs(across.y) < ComboMove::kRise);
    PlayScene::Inputs grab{};
    grab[1].combo = true;
    grab[1].aimDirection = Vec3{across.x, 0.0f, across.z};
    for (u32 guard = 0; guard < 30 && !scene.runtime(0)->combo.active(); ++guard) {
        fixture->step(grab);
    }
    from = record.calls.size();
    REQUIRE(scene.runtime(1)->combo.role == ComboRole::Grabber);
    REQUIRE(PartyCombo::aside(*scene.runtime(0)));
    u32 heldTicks = 0;
    for (; heldTicks < 600 && PartyCombo::aside(*scene.runtime(0)); ++heldTicks) {
        CHECK(press(Press::Down) == std::vector{1});
    }
    REQUIRE_FALSE(PartyCombo::aside(*scene.runtime(0)));
    CHECK(heldTicks > 1);
    CHECK(press(Press::Left) == std::vector{0, 1});

    // Nor does one down: from its fall, through its death, to the question in its box.
    REQUIRE(scene.kill(1));
    u32 downTicks = 0;
    for (; downTicks < 900 && !scene.runtime(1)->towerPrompt; ++downTicks) {
        CHECK(press(Press::Right) == std::vector{0});
    }
    REQUIRE(scene.runtime(1)->towerPrompt);
    CHECK(scene.runtime(1)->life == PlayerLife::InTower);
    CHECK(downTicks > 1);
    // Nor one that has answered it by leaving the game.
    PlayScene::Inputs quit{};
    quit[1].menu.back = true;
    CHECK(press(Press::Down, quit) == std::vector{0});
    REQUIRE(scene.runtime(1)->departed);
    CHECK(press(Press::Left) == std::vector{0});

    // A look the settings choose now opens for every member the scene draws, each in its own
    // class and colour, the one gone from the game too, whose box still shows its colour.
    fixture->config.display.hudStyle = std::string{kOtherLook};
    CHECK(press(Press::Right) == std::vector{0});
    REQUIRE(fixture->styles.opened.back() == kOtherLook);
    CHECK(fixture->styles.parties.back() ==
          std::vector<HudMember>{{.seat = 0, .classIndex = kWarriorClass, .color = 2},
                                 {.seat = 1, .classIndex = kWizardClass, .color = 1}});
}

TEST_CASE("a look that throws in play leaves the scene drawing Dark Legacy's",
          "[play-scene][hud-style][assets]") {
    auto fixture = std::make_unique<HudScene>("G1");
    fixture->config.display.bloom = true; // where the bloom ran, the overlay begins
    fixture->options.position = Vec3{10.7f, 10.2f, -60.5f}; // open ground
    fixture->styles.throwsIn = FakeHudPart::Status;
    fixture->open(kFakeLook);
    fixture->settle();
    PlayScene& scene = fixture->scene;
    const test::FakeHudRecord& record = fixture->styles.record;

    // The boxes as Dark Legacy's look draws them for the scene's party, to be found among the
    // scene's own draws.
    test::FakeRenderDevice expectedDevice;
    DarkLegacyHudPainter expected;
    REQUIRE(expected.load(expectedDevice, fixture->root, &fixture->strings));
    const PowerupSelector closed;
    const PickupHud noPickups;
    const ClassDataSet classes;
    const Mat4 frameProjection = makeScreenProjection(640, 448);
    const Mat4 canvasProjection = makeVirtualScreenTransform(frameProjection, 512, 384, 640, 448);
    const auto boxes = [&] {
        HudView view;
        view.players = scene.participants();
        view.pickups = &noPickups;
        for (usize seat = 0; seat < view.seats.size(); ++seat) {
            view.seats.at(seat) = hudSeatOf(static_cast<s32>(seat), scene.participants(), closed,
                                            classes, {.keysShown = scene.hud().relicsShown()});
        }
        expectedDevice.draws.clear();
        Canvas canvas;
        HudFrame frame{.canvas = canvas, .device = expectedDevice, .darkLegacy = expected};
        canvas.begin(expectedDevice, canvasProjection);
        expected.drawStatus(frame, view);
        canvas.end();
        return verticesOf(expectedDevice.draws);
    };
    const auto overlay = [&] {
        fixture->render();
        REQUIRE(fixture->device.bloomDrawOffsets.size() == 1);
        return verticesOf(
            std::span{fixture->device.draws}.subspan(fixture->device.bloomDrawOffsets.front()));
    };

    // The frame it throws in says so once, and its boxes go undrawn.
    std::vector<ImmediateVertex> thrown;
    const std::vector<std::string> warnings = test::warningsWhile([&] { thrown = overlay(); });
    CHECK(warnings == std::vector<std::string>{"HUD: the fake look failed (the fake look broke); "
                                               "drawing Dark Legacy's for the rest of the scene"});
    CHECK(record.calls.back() ==
          test::FakeHudCall{.look = std::string{kFakeLook}, .part = FakeHudPart::Status});
    const std::vector<ImmediateVertex> darkLegacyBoxes = boxes();
    REQUIRE_FALSE(darkLegacyBoxes.empty());
    CHECK_FALSE(holds(thrown, darkLegacyBoxes));
    // From the next frame Dark Legacy's look draws the boxes, and the retired look is asked
    // nothing; it is kept until a step, since the frames in flight may still draw with it.
    const usize calls = record.calls.size();
    CHECK(holds(overlay(), darkLegacyBoxes));
    CHECK(record.calls.size() == calls);
    CHECK(record.departures == 0);

    // The next step waits for the device, lets it go and opens nothing again.
    const u32 waits = fixture->device.idleWaits;
    fixture->step({});
    CHECK(fixture->device.idleWaits == waits + 1);
    REQUIRE(record.destroyed() == std::vector{std::string{kFakeLook}});
    CHECK(record.idleWaitsAtLetGo.front() > waits);
    CHECK(fixture->styles.opened == std::vector{std::string{kFakeLook}});
    fixture->step({});
    CHECK(fixture->device.idleWaits == waits + 1);
    CHECK(holds(overlay(), boxes()));
    expected.clear();
}

TEST_CASE("a seat standing on a tower portal is told that portal in its own seat's view",
          "[play-scene][hud-style][assets]") {
    // Two players, the second seat first in the party, each of a class other than its colour.
    auto fixture = std::make_unique<HudScene>("L1");
    std::ranges::reverse(fixture->party);
    fixture->party[0].save.character = kArcherClass;
    fixture->party[0].save.color = 2;
    fixture->party[1].save.color = 3;
    fixture->open(kFakeLook);
    // The look opened for each member's own class and colour, in the party's order.
    CHECK(fixture->styles.parties.front() ==
          std::vector<HudMember>{{.seat = 1, .classIndex = kArcherClass, .color = 2},
                                 {.seat = 0, .classIndex = kWarriorClass, .color = 3}});
    fixture->settle();
    PlayScene& scene = fixture->scene;
    REQUIRE(scene.participants().front().actor.player() == 1);
    const ExitPortals& exits = scene.portals();
    usize g1 = 0;
    while (g1 < exits.size() &&
           (!exits.portal(g1).destination || exits.portal(g1).destination->name != "G1")) {
        ++g1;
    }
    REQUIRE(g1 < exits.size());
    const ExitPortals::Portal& portal = exits.portal(g1);

    // The second seat steps onto the portal to G1; the first stays where it came in.
    REQUIRE(scene.actor(1) != nullptr);
    REQUIRE(scene.noclipMove(1, portal.position - scene.actor(1)->position(), std::nullopt));
    const test::FakeHudRecord& record = fixture->styles.record;
    // What a frame's view tells the look of each seat's exit.
    const auto told = [&] {
        fixture->render();
        CHECK_FALSE(record.pads.at(0).has_value());
        REQUIRE(record.pads.at(1).has_value());
        const test::FakeHudPad& pad = *record.pads.at(1);
        CHECK(pad.identity == kDarkLegacyPadIdentityBase + static_cast<u32>(g1));
        CHECK_FALSE(pad.nativeCode.has_value());
        REQUIRE(pad.destination.has_value());
        CHECK(pad.destination->name == "G1");
        CHECK(*pad.destination == *portal.destination);
    };
    told();
    // A tick on it stands there still, and so does what the HUD is told.
    fixture->step({});
    told();
}

TEST_CASE("a companion's arrow keeps above the panels of the look the HUD is drawn in",
          "[play-scene][hud-style][assets]") {
    auto fixture = std::make_unique<HudScene>("L1");
    fixture->open(kFakeLook);
    fixture->settle();
    PlayScene& scene = fixture->scene;
    // The second seat plays on another machine, so this one is shown the way to it.
    scene.setLocalSeats(0b01);
    // The lowest point of the arrow a frame draws to the second seat's character, stood just
    // behind the eye: the arrow points to it down the view, as low as the look's panels allow.
    const auto arrowBottom = [&] {
        const WorldCamera eye = scene.viewCamera();
        REQUIRE(scene.actor(1) != nullptr);
        REQUIRE(scene.noclipMove(
            1, eye.position - eye.forward() * kBehindEye - scene.actor(1)->position(),
            std::nullopt));
        fixture->render();
        REQUIRE(scene.companionArrows().shown() == 1);
        std::optional<f32> bottom;
        for (const test::RecordedDraw& draw : fixture->device.draws) {
            // The arrow is a triangle in its character's colour; its dark rim is left out.
            if (draw.texture != &fixture->device.whiteTexture() || draw.vertices.size() != 3) {
                continue;
            }
            const Color color = draw.vertices.front().color;
            if (color.a != CompanionArrows::kAlpha ||
                (color.r == 0 && color.g == 0 && color.b == 0)) {
                continue;
            }
            for (const ImmediateVertex& vertex : draw.vertices) {
                bottom = std::max(bottom.value_or(vertex.position.y), vertex.position.y);
            }
        }
        REQUIRE(bottom.has_value());
        return *bottom;
    };
    CHECK(arrowBottom() ==
          Catch::Approx(test::FakeHudPainter::kStatusTop - CompanionArrows::kInset));
    // Back in Dark Legacy's look, the arrow keeps above its boxes.
    fixture->config.display.hudStyle = std::string{kDarkLegacyHudStyle};
    fixture->step({});
    CHECK(arrowBottom() ==
          Catch::Approx(static_cast<f32>(StatusBoxPainter::kBarY) - CompanionArrows::kInset));
}
