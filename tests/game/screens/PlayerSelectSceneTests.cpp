#include <cmath>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/assets/StringTable.h"
#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundClip.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/math/Math.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/menu/MenuInput.h"
#include "game/screens/GameContext.h"
#include "game/screens/PlayerSelectScene.h"

namespace {

using namespace gdl;
using namespace gdl::game;

std::filesystem::path unpackedRoot() {
    return test::unpackedOrSkip("SELECT/textures.json").parent_path().parent_path();
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

TEST_CASE("the starting player joins and others join on Start", "[game][select][unpacked]") {
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

TEST_CASE("backing out of the last lane cancels the screen", "[game][select][unpacked]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-cancel");
    PlayerSelectScene scene;
    const auto context = f.context();
    REQUIRE(scene.open(device, context, 0));
    REQUIRE(scene.step(1, player(0, false, true)) == SelectOutcome::Cancelled);
}

TEST_CASE("the screen finishes once every player is locked in", "[game][select][unpacked]") {
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
    int frames = 0;
    while (outcome == SelectOutcome::Running && frames < 200) {
        outcome = scene.step(1, nobody());
        ++frames;
    }
    REQUIRE(outcome == SelectOutcome::Done);
    REQUIRE(frames > PlayerSelectScene::kIdleFrames);
}

TEST_CASE("Sumner greets a locked-in character by costume and class", "[game][select][unpacked]") {
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
        const auto chunks = static_cast<int>(std::ceil(seconds * 10.0));
        for (int i = 0; i < chunks; ++i) {
            mixer.mix(out);
            sounds.update();
        }
    };
    drain(welcomeSeconds + 0.5);
    REQUIRE(scene.speaking());
    drain(10.0);
    REQUIRE_FALSE(scene.speaking());
}

} // namespace
