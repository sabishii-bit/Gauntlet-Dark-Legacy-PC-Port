#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
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

    GameContext context(std::filesystem::path root = unpackedRoot()) const {
        GameContext out;
        out.config = &config;
        out.strings = &strings;
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
    REQUIRE(scene.open(device, f.context(), 2));
    REQUIRE(scene.lane(2).active());
    REQUIRE_FALSE(scene.lane(0).active());
    REQUIRE(scene.step(1, nobody()) == SelectOutcome::Running);
    REQUIRE(scene.step(1, player(0, false, false, true)) == SelectOutcome::Running);
    REQUIRE(scene.lane(0).active());
    REQUIRE(scene.lane(0).state() == SelectLane::State::TopMenu);

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
    REQUIRE(scene.open(device, f.context(), 0));
    REQUIRE(scene.step(1, player(0, false, true)) == SelectOutcome::Cancelled);
}

TEST_CASE("the screen finishes once every player is locked in", "[game][select][unpacked]") {
    test::FakeRenderDevice device;
    const Fixture f("select-scene-done");
    PlayerSelectScene scene;
    REQUIRE(scene.open(device, f.context(), 0));
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

} // namespace
