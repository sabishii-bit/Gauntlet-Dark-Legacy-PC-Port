#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundClip.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/menu/MenuInput.h"
#include "game/players/Party.h"
#include "game/screens/GameContext.h"
#include "game/screens/PlayerSelectScene.h"

namespace {

using namespace gdl;
using namespace gdl::game;

std::filesystem::path unpackedRoot() {
    return test::assetOrSkip("SELECT/textures.ngc").parent_path().parent_path();
}

struct Fixture {
    GameConfig config;
    StringTable strings;

    explicit Fixture(std::string_view scratch) {
        config.save.directory = test::scratchDirectory(scratch).string();
        config.save.slots = 4;
        strings.load(test::dataDirectory() / "text", config.text.language);
    }

    GameContext context(std::filesystem::path root = unpackedRoot(),
                        SoundPlayer* sounds = nullptr) const {
        GameContext out;
        out.config = &config;
        out.strings = &strings;
        out.sounds = sounds;
        out.unpackedRoot = std::move(root);
        return out;
    }
};

PlayerSelectScene::Inputs nobody() {
    return {};
}

PlayerSelectScene::Inputs player(s32 index, bool select, bool back = false, bool start = false,
                                 bool right = false) {
    PlayerSelectScene::Inputs inputs{};
    inputs[static_cast<usize>(index)].select = select;
    inputs[static_cast<usize>(index)].back = back;
    inputs[static_cast<usize>(index)].start = start;
    inputs[static_cast<usize>(index)].right = right;
    return inputs;
}

TEST_CASE("the select screen refuses to open without unpacked data", "[game][select]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-empty");
    PlayerSelectScene scene;
    REQUIRE_FALSE(scene.open(device, f.context(test::scratchDirectory("select-none")), 0));
    REQUIRE_FALSE(scene.isOpen());
}

TEST_CASE("the starting player joins and others join on Start", "[game][select][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-join");
    PlayerSelectScene scene;
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 2));
    REQUIRE(scene.lane(2).active());
    REQUIRE_FALSE(scene.lane(0).active());
    REQUIRE(scene.step(1, nobody()) == SelectOutcome::Running);
    REQUIRE(scene.step(1, player(0, false, false, true)) == SelectOutcome::Running);
    REQUIRE(scene.lane(0).active());
    REQUIRE(scene.lane(0).state() == SelectLane::State::TopMenu);
    REQUIRE(scene.inputSource(0).keyboard);
    REQUIRE_FALSE(scene.inputSource(0).text);
    REQUIRE(scene.step(1, player(0, true)) == SelectOutcome::Running); // New
    REQUIRE(scene.lane(0).state() == SelectLane::State::NameEntry);
    REQUIRE(scene.inputSource(0).text);
    REQUIRE_FALSE(scene.inputSource(2).text);

    const Mat4 projection = makeScreenProjection(640.0f, 448.0f);
    scene.render(device, projection, 640.0f, 448.0f);
    REQUIRE(device.draws.size() >= 8);
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
}

TEST_CASE("Manage Character opens its player's save menu with the rest of the party retained",
          "[game][select][pause][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-manage");
    PlayerSelectScene scene;
    CharacterSave first;
    first.name = "FIRST";
    first.gold = 123;
    CharacterSave second;
    second.name = "SECOND";
    second.gold = 456;
    const std::array party{PartyMember{1, first, 0}, PartyMember{3, second, 2}};
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 3, party, true));
    REQUIRE(scene.lane(1).lockedIn());
    REQUIRE(scene.lane(1).save().toJson() == first.toJson());
    REQUIRE(scene.lane(1).slotInUse() == 0);
    REQUIRE(scene.lane(3).state() == SelectLane::State::SaveMenu);
    REQUIRE(scene.lane(3).save().toJson() == second.toJson());
    REQUIRE(scene.lane(3).slotInUse() == 2);
    REQUIRE_FALSE(scene.lane(0).active());
    REQUIRE(scene.step(1, nobody()) == SelectOutcome::Running);
    scene.close();
}

TEST_CASE("backing out of the last lane cancels the screen", "[game][select][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-cancel");
    PlayerSelectScene scene;
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 0));
    REQUIRE(scene.step(1, player(0, false, true)) == SelectOutcome::Cancelled);
}

TEST_CASE("the screen finishes once every player is locked in", "[game][select][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-done");
    PlayerSelectScene scene;
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 0));
    scene.step(1, player(0, true)); // New
    REQUIRE(scene.lane(0).state() == SelectLane::State::NameEntry);
    scene.step(1, player(0, true)); // accept the end mark: a random name
    scene.step(NameEntry::kFlashTicks + 1, nobody());
    REQUIRE(scene.lane(0).state() == SelectLane::State::ClassPick);
    scene.step(1, player(0, false, false, false, true));
    REQUIRE(scene.lane(0).pickedClass() == 1);
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).lockedIn());
    SelectOutcome outcome = SelectOutcome::Running;
    s32 frames = 0;
    while (outcome == SelectOutcome::Running && frames < 200) {
        outcome = scene.step(1, nobody());
        ++frames;
    }
    REQUIRE(outcome == SelectOutcome::Done);
    REQUIRE(frames > PlayerSelectScene::kIdleFrames);
}

TEST_CASE("a player joining a game in progress finds the party locked in beside them",
          "[game][select][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-join");
    PlayerSelectScene scene;
    const auto context = f.context();
    CharacterSave playing;
    playing.name = "ALREADY";
    playing.character = 1;
    const std::array party{PartyMember{1, playing, std::optional<usize>{0}}};
    REQUIRE(scene.open(device, context, 0, party));
    CHECK(scene.lane(1).lockedIn());
    CHECK(scene.lane(1).save().name == "ALREADY");
    CHECK(scene.lane(1).slotInUse() == std::optional<usize>{0});
    CHECK(scene.lane(0).state() == SelectLane::State::TopMenu);
    // The joiner thinking better of it leaves the party as it was.
    scene.step(1, player(0, false, true));
    CHECK_FALSE(scene.lane(0).active());
    SelectOutcome outcome = SelectOutcome::Running;
    for (s32 frame = 0; frame < 200 && outcome == SelectOutcome::Running; ++frame) {
        outcome = scene.step(1, nobody());
    }
    CHECK(outcome == SelectOutcome::Done);
    CHECK(scene.lane(1).lockedIn());
}

TEST_CASE("Sumner greets a locked-in character by costume and class", "[game][select][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-greeting");
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    PlayerSelectScene scene;
    const auto context = f.context(unpackedRoot(), &sounds);
    REQUIRE(scene.open(device, context, 0));
    REQUIRE_FALSE(scene.speaking());
    const usize before = sounds.voiceCount(); // the music, when the bank is there
    scene.step(1, player(0, true));           // New
    scene.step(1, player(0, true));           // a random name
    scene.step(NameEntry::kFlashTicks + 1, nobody());
    scene.step(1, player(0, true)); // lock in the warrior
    REQUIRE(scene.lane(0).lockedIn());
    REQUIRE(scene.speaking());
    REQUIRE(sounds.voiceCount() > before); // the welcome (and the select click)

    // The name line follows the welcome, so Sumner is still speaking once the welcome alone
    // would have ended, and quiet some seconds later.
    SoundSet bank;
    REQUIRE(bank.load(unpackedRoot() / "audio" / "SELECT"));
    const SoundSequence welcome = bank.sequence(bank.find("S_WELCOME").value());
    f64 welcomeSeconds = 0.0;
    for (const SoundSequenceStep& step : welcome.steps) {
        welcomeSeconds += step.clip->seconds();
    }
    std::vector<f32> out(usize{4800} * 2); // a tenth of a second of stereo
    const auto drain = [&](f64 seconds) {
        const auto chunks = static_cast<s32>(std::ceil(seconds * 10.0));
        for (s32 i = 0; i < chunks; ++i) {
            mixer.mix(out);
            sounds.update();
        }
    };
    drain(welcomeSeconds + 0.5);
    REQUIRE(scene.speaking());
    drain(10.0);
    REQUIRE_FALSE(scene.speaking());
}

TEST_CASE("post-shop prompts surviving lanes and retains fallen character checkpoints",
          "[game][select][post-shop][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-after-shop");
    PlayerSelectScene scene;
    CharacterSave alive;
    alive.name = "ALIVE";
    alive.gold = 4321;
    CharacterSave fallen;
    fallen.name = "FALLEN";
    const std::array party{PartyMember{0, alive, std::nullopt, false, 0, {14, 27}},
                           PartyMember{2, alive, 1}, PartyMember{3, fallen, 2, true}};
    REQUIRE(scene.openAfterLevel(device, f.context(), party));
    CHECK(scene.lane(0).state() == SelectLane::State::SaveMenu);
    CHECK(scene.lane(2).state() == SelectLane::State::SaveMenu);
    CHECK(scene.lane(3).lockedIn());
    CHECK_FALSE(scene.lane(1).active());
    CHECK(scene.step(60, player(0, false, true)) == SelectOutcome::Running);
    CHECK(scene.lane(0).state() == SelectLane::State::SaveMenu);
    scene.step(1, player(0, true)); // Done; the other survivor still owns its menu.
    CHECK(scene.lane(0).lockedIn());
    CHECK(scene.step(60, nobody()) == SelectOutcome::Running);
    scene.step(1, player(2, true));
    SelectOutcome outcome = SelectOutcome::Running;
    for (s32 i = 0; i <= PlayerSelectScene::kIdleFrames; ++i) {
        outcome = scene.step(1, nobody());
    }
    CHECK(outcome == SelectOutcome::Done);
    const auto result = scene.party();
    REQUIRE(result.size() == 3);
    CHECK(result[0].save.toJson() == alive.toJson());
    CHECK(result[0].helpHeard == std::vector<s32>{14, 27});
    CHECK(result[1].slot == 1);
    CHECK(result[2].fallen);
    CHECK(result[2].save.toJson() == fallen.toJson());
}

TEST_CASE("post-shop lanes cannot race for another lane's pending save slot",
          "[game][select][post-shop][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-after-shop-reservations");
    PlayerSelectScene scene;
    CharacterSave first;
    first.name = "FIRST";
    CharacterSave second;
    second.name = "SECOND";
    const std::array party{PartyMember{0, first, std::nullopt, false, 0, {14}},
                           PartyMember{1, second}};
    REQUIRE(scene.openAfterLevel(device, f.context(), party));
    PlayerSelectScene::Inputs both{};
    both[0].up = both[1].up = true;
    for (s32 i = 0; i < 3; ++i) {
        scene.step(1, both); // Done -> Quit -> Change -> Save; no files to load.
    }
    both = {};
    both[0].select = both[1].select = true;
    scene.step(1, both);
    REQUIRE(scene.lane(0).state() == SelectLane::State::SavePick);
    REQUIRE(scene.lane(1).state() == SelectLane::State::SavePick);
    scene.step(1, both);
    CHECK(scene.lane(0).state() == SelectLane::State::Saving);
    CHECK(scene.lane(1).state() == SelectLane::State::SavePick);
    CHECK(scene.lane(0).reservedSlot() == 0);
    scene.step(SelectLane::kOperationStepTicks * 3, nobody());
    CharacterSave saved;
    REQUIRE(scene.saves().load(0, saved));
    CHECK(saved.name == "FIRST");
    CHECK(scene.party()[0].slot == 0);
    CHECK(scene.party()[0].helpHeard == std::vector<s32>{14});
    CHECK_FALSE(scene.party()[1].slot.has_value());
    scene.step(1, player(1, true));
    CHECK(scene.lane(1).state() == SelectLane::State::SavePick);
}

TEST_CASE("post-shop keeps Done selected and protects an existing save when overwrite is refused",
          "[game][select][post-shop][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-after-shop-focus");
    SaveSlots slots;
    REQUIRE(slots.open(f.config.saveDirectory(), f.config.save.slots));
    CharacterSave save;
    save.name = "EXIST";
    REQUIRE(slots.write(0, save));
    save.gold = 4567;
    const std::array party{PartyMember{0, save, 0}};
    PlayerSelectScene scene;
    REQUIRE(scene.openAfterLevel(device, f.context(), party));
    // The GameCube card-directory probe returns at most one, selecting Done.
    PlayerSelectScene::Inputs up{};
    up[0].up = true;
    for (s32 i = 0; i < 4; ++i) {
        scene.step(1, up);
    }
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).state() == SelectLane::State::SavePick);
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).state() == SelectLane::State::OverwriteConfirm);
    scene.step(1, player(0, true)); // No is the default; leave the old file alone.
    CharacterSave before;
    REQUIRE(slots.load(0, before));
    CHECK(before.gold != save.gold);
    CHECK(scene.party()[0].save.gold == save.gold);
}

TEST_CASE("post-shop snapshots follow a loaded character rather than the pending journey",
          "[game][select][post-shop][assets]") {
    test::FakeRenderDevice device;
    const Fixture f("select-after-shop-load");
    SaveSlots slots;
    REQUIRE(slots.open(f.config.saveDirectory(), f.config.save.slots));
    CharacterSave saved;
    saved.name = "SAME";
    saved.gold = 250;
    REQUIRE(slots.write(0, saved));
    CharacterSave live = saved;
    live.gold = 600;
    const std::array party{PartyMember{0, live, 0, false, 0, {14}}};
    PlayerSelectScene scene;
    REQUIRE(scene.openAfterLevel(device, f.context(), party));
    PlayerSelectScene::Inputs up{};
    up[0].up = true;
    scene.step(1, up); // Quit
    scene.step(1, up); // Load
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).state() == SelectLane::State::LoadPick);
    scene.step(1, player(0, true));
    REQUIRE(scene.lane(0).state() == SelectLane::State::Loading);
    CHECK(scene.party()[0].save.gold == 600); // No speculative replacement.
    scene.step(SelectLane::kOperationStepTicks * 3, nobody());
    REQUIRE(scene.party().size() == 1);
    CHECK(scene.party()[0].save.gold == 250);
    CHECK(scene.party()[0].helpHeard.empty()); // Loading resets even an identical name/class.
    scene.step(SelectLane::kNoticeTicks, nobody());
    REQUIRE(scene.lane(0).state() == SelectLane::State::ClassPick);
    scene.step(1, player(0, false, false, false, true));
    CHECK(scene.lane(0).pickedClass() == 1);
    CHECK(scene.party()[0].save.character == 0); // Browsing has not committed the choice.
}

} // namespace
