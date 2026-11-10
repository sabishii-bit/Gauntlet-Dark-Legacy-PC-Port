#include <algorithm>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/level/LegendsDreamcastGates.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast Temple entrance doors emit their authored movement sound",
          "[legends][dreamcast-gates][dreamcast-temple-door][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    const auto directory = root / "levels/levele1";
    if (!std::filesystem::exists(directory / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    const auto bytes = readFile(directory / "worlds.rom");
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(scene);
    const auto* motion = scene->dreamcastWorldMotion();
    REQUIRE(motion);
    const auto pad = std::ranges::find(motion->switches(), 94U,
                                       &LegendsDreamcastWorldMotion::Switch::nativeIndex);
    REQUIRE(pad != motion->switches().end());
    REQUIRE(pad->target == 73); // E1ANIM100; chained pad 95 controls E1ANIM101
    const usize index = static_cast<usize>(pad - motion->switches().begin());
    const Vec3 position = motion->switchPosition(index);
    LegendsDreamcastSounds sounds;
    REQUIRE(sounds.open(root));
    const auto pair = sounds.motionTokens('E', 0);
    REQUIRE(pair);
    CHECK((*pair)[0] == 0x2b001c); // native fallback S_ELVROPEE
    CHECK((*pair)[1] == 0x2b001d);
    scene->update(4); // finish any startup reverse poses before touching the doorway
    scene->takeSounds();
    const std::array visitors{LegendsPortalVisitor{.position = position}};
    scene->touchSwitches(visitors);
    REQUIRE(pad->active);
    scene->update(1.f / 60);
    const auto loops = scene->soundLoops();
    const auto sound = std::ranges::find(loops, (*pair)[0], &LegendsSoundLoop::token);
    REQUIRE(sound != loops.end());
    // Nested door groups have local coordinates close to zero but stand at
    // z92. The cue must follow the world position, where the player can hear it.
    CHECK(glm::distance(sound->position, position) < 15.f);
    for (s32 frame = 0; frame < 600; ++frame) {
        scene->update(1.f / 60);
    }
    const auto stopped = scene->takeSounds();
    CHECK(
        std::ranges::any_of(stopped, [&](const auto& event) { return event.token == (*pair)[1]; }));
}

formats::ItemInfoRecord keyDoor() {
    formats::ItemInfoRecord info;
    info.type = 7;
    info.name = "GATEA";
    info.activeType = 0x46;
    info.collisionType = 3;
    info.radius = 6.5f;
    info.xSize = 6;
    info.zSize = 1;
    info.height = 3;
    return info;
}

TEST_CASE("Dreamcast key doors use native half extents without N64 width conversion",
          "[legends][dreamcast-gates]") {
    formats::ItemInstanceRecord instance;
    instance.position = {20, 4, 10};
    instance.rotation.y = kHalfPi;
    instance.rotation.x = -1.0e-7f;
    auto definition = legendsDreamcastGate(keyDoor(), instance, 23);
    REQUIRE(definition);
    CHECK(definition->nativeIndex == 23);
    CHECK(definition->sound == 0);
    CHECK_FALSE(definition->initiallyActive);
    LegendsGate gate{*definition};
    auto body = gate.primaryObstacle();
    CHECK(body.centre == instance.position);
    CHECK(body.yaw == kHalfPi);
    CHECK(body.halfAcross == 6);
    CHECK(body.halfAlong == 1);
    CHECK(body.height == 3);
    CHECK_FALSE(gate.frameObstacles()[0].solid);
    CHECK_FALSE(gate.frameObstacles()[1].solid);
    s32 keys = 2;
    REQUIRE(gate.touch({0, {21.5f, 4, 10}, true, .75f, 5}, keys));
    CHECK(keys == 1);
    CHECK_FALSE(gate.primaryObstacle().solid);
    CHECK(gate.state() == LegendsGate::State::Opening);
    CHECK_FALSE(gate.touch({1, {21.5f, 4, 10}, true, .75f, 5}, keys));
    CHECK(keys == 1);
    CHECK(gate.finishOpening());
    CHECK_FALSE(gate.finishOpening());
    CHECK(gate.state() == LegendsGate::State::Open);
}

TEST_CASE("Dreamcast gates refuse invalid key contact and do not spend while retreating",
          "[legends][dreamcast-gates]") {
    auto definition = legendsDreamcastGate(keyDoor(), {}, 0);
    REQUIRE(definition);
    LegendsGate gate{*definition};
    s32 keys = 0;
    CHECK_FALSE(gate.touch({0, {0, 0, 1.25f}, true, .75f, 5}, keys));
    keys = 2;
    CHECK_FALSE(gate.touch({0, {0, 0, 1.5f}, true, .75f, 5}, keys));
    CHECK(keys == 2);
    // Another player's position history cannot change this player's direction.
    CHECK_FALSE(gate.touch({1, {30, 0, 30}, true, .75f, 5}, keys));
    CHECK_FALSE(gate.touch({0, {0, 0, 1.4f}, false, .75f, 5}, keys));
    CHECK_FALSE(gate.touch({0, {0, 4, 1.4f}, true, .75f, 5}, keys));
    CHECK_FALSE(gate.touch({4, {0, 0, 1.4f}, true, .75f, 5}, keys));
    CHECK(keys == 2);
    REQUIRE(gate.touch({0, {0, 0, 1.3f}, true, .75f, 5}, keys));
    CHECK(keys == 1);
}

TEST_CASE("Unsupported Dreamcast gate records remain explicit rather than becoming free doors",
          "[legends][dreamcast-gates]") {
    auto info = keyDoor();
    formats::ItemInstanceRecord instance;
    SECTION("scripted triangle column") {
        info.collisionType = 4;
        info.activeType = 6;
        instance.triangleCount = 16;
    }
    SECTION("nonfinite extent") {
        info.xSize = std::numeric_limits<f32>::quiet_NaN();
    }
    SECTION("nonfinite placement") {
        instance.position.z = std::numeric_limits<f32>::infinity();
    }
    SECTION("hidden trigger") {
        instance.flags = 2;
    }
    SECTION("tilted gate") {
        instance.rotation.x = .2f;
    }
    SECTION("different state machine") {
        info.activeType = 6;
    }
    std::string diagnostic;
    CHECK_FALSE(legendsDreamcastGate(info, instance, 0, &diagnostic));
    CHECK_FALSE(diagnostic.empty());
    info.type = 1;
    CHECK_FALSE(legendsDreamcastGate(info, instance, 0, &diagnostic));
    CHECK(diagnostic.empty());
}

TEST_CASE("Every shipped ordinary Dreamcast key door passes its native adapter",
          "[legends][dreamcast-gates][assets]") {
    const auto levels =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::is_directory(levels)) {
        SKIP("Dreamcast extraction is not installed");
    }
    usize ordinary = 0;
    usize scripted = 0;
    for (const auto& directory : std::filesystem::directory_iterator(levels)) {
        const auto path = directory.path() / "worlds.rom";
        if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) == 0) {
            continue;
        }
        const auto scene = formats::LegendsDreamcastScene::parse(readFile(path));
        for (usize index = 0; index < scene.itemInstances.size(); ++index) {
            const auto& instance = scene.itemInstances[index];
            if (instance.info < 0) {
                continue;
            }
            const auto& info = scene.itemInfos[static_cast<usize>(instance.info)];
            if (info.type != 7) {
                continue;
            }
            INFO(path.string() << " placement " << index << " " << info.name);
            std::string diagnostic;
            const auto gate =
                legendsDreamcastGate(info, instance, static_cast<u32>(index), &diagnostic);
            if (info.collisionType == 4) {
                ++scripted;
                CHECK_FALSE(gate);
                CHECK_FALSE(diagnostic.empty());
            } else {
                ++ordinary;
                REQUIRE(gate);
                CHECK(diagnostic.empty());
                CHECK(gate->primary.halfAcross == info.xSize);
                CHECK(gate->primary.halfAlong == info.zSize);
            }
        }
    }
    CHECK(ordinary == 103);
    CHECK(scripted == 3);
}

} // namespace
