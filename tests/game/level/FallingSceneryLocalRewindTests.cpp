#include <array>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/level/FallingScenery.h"
#include "game/level/NativeStateResources.h"
#include "game/rollback/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;

void checkCues(std::span<const FallingCue> a, std::span<const FallingCue> b) {
    REQUIRE(a.size() == b.size());
    for (usize index = 0; index < a.size(); ++index) {
        CHECK((std::tie(a[index].position, a[index].sound) ==
               std::tie(b[index].position, b[index].sound)));
    }
}

/** Whether `scenery` now holds exactly the values `saved` holds. */
bool holds(const FallingScenery& scenery, const FallingScenery::LocalSnapshot& saved) {
    FallingScenery::LocalSnapshot now;
    scenery.saveLocal(now);
    return scenery.equalLocal(now, saved);
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    FallingScenery scenery;
    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0]}],
          "bounds":{"min":[-20,-10,-20],"max":[400,20,20]},
          "itemInfos":[
            {"type":10,"subtype":40,"radius":2,"height":3},
            {"type":10,"subtype":49,"radius":2,"height":3},
            {"type":10,"subtype":52,"radius":2,"height":3},
            {"type":10,"subtype":53,"radius":2,"height":3}],
          "itemInstances":[
            {"info":0,"name":"WALL","position":[0,0,0]},
            {"info":1,"name":"WALL","position":[20,0,0]},
            {"info":2,"name":"WALL","position":[40,0,0]},
            {"info":3,"name":"WALL","position":[60,0,0],"minPlayers":2}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(models.load(directory));
        REQUIRE(textures.load(directory));
        scenery.bind(device, layout, models, textures, "D2");
        REQUIRE(scenery.size() == 4);
    }
    /** The scenery's native digest over its own stage, so two fixtures can be compared. */
    NativeStateDigest::Digest digest() const {
        NativeStateResources resources;
        REQUIRE(resources.registerLayout(layout, "stage"));
        REQUIRE(resources.registerTextures(textures, "stage/textures"));
        return test::liveDigest(scenery, resources.resources());
    }
    std::vector<FallingCue> step(s32 tick, f32 seconds) {
        std::vector<FallingCue> result;
        if (tick == 0) {
            REQUIRE(scenery.touch({40, 0, 0}, 1).empty());
        }
        if (tick <= 3) {
            scenery.setPlayerCount(2);
            result = tick == 2 ? scenery.shoot({40, 0, 0}, 1)
                               : scenery.touch({20.0f * static_cast<f32>(tick), 0, 0}, 1);
            REQUIRE(result.size() == 1);
        }
        scenery.update(seconds);
        return result;
    }
};

TEST_CASE("falling checkpoint resumes native profiles cues half frames and retirement",
          "[falling-local-rewind]") {
    Fixture host("falling-checkpoint-host");
    Fixture guest("falling-checkpoint-guest");
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const auto uploads = guest.device.texturesCreated;
    const auto* first = &host.scenery.piece(0);
    usize cues = 0;
    bool distinctPrevious = false;
    FallingScenery::LocalSnapshot before;
    FallingScenery::LocalSnapshot expected;
    for (s32 tick = 0; tick < 240; ++tick) {
        CAPTURE(tick, seconds);
        host.scenery.saveLocal(before);
        const auto output = host.step(tick, seconds);
        cues += output.size();
        host.scenery.saveLocal(expected);
        const auto& motion = host.scenery.piece(0).motion;
        distinctPrevious |= motion.presentedPosition(0) != motion.position;
        checkCues(guest.step(tick, seconds), output);
        CHECK(guest.digest() == host.digest());
        for (const f32 alpha : {0.0f, 0.5f, 1.0f}) {
            host.device.draws.clear();
            guest.device.draws.clear();
            host.scenery.draw(host.device, Mat4{1}, {}, alpha);
            guest.scenery.draw(guest.device, Mat4{1}, {}, alpha);
            REQUIRE(host.device.draws.size() == guest.device.draws.size());
            for (usize draw = 0; draw < host.device.draws.size(); ++draw) {
                CHECK(host.device.draws[draw].vertices == guest.device.draws[draw].vertices);
                CHECK(host.device.draws[draw].transform == guest.device.draws[draw].transform);
            }
        }
        REQUIRE(host.scenery.restoreLocal(before));
        CHECK(&host.scenery.piece(0) == first);
        CHECK(holds(host.scenery, before));
        checkCues(host.step(tick, seconds), output);
        CHECK(holds(host.scenery, expected));
    }
    CHECK(cues == 4);
    CHECK(distinctPrevious);
    for (usize index = 0; index < host.scenery.size(); ++index) {
        CHECK_FALSE(host.scenery.piece(index).motion.visible);
    }
    CHECK(guest.device.texturesCreated == uploads);
}

TEST_CASE("native forest falling checkpoint retains the full original model roster",
          "[falling-local-rewind][assets]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELF1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    FallingScenery scenery;
    scenery.bind(device, layout, models, textures, "F1");
    REQUIRE(scenery.size() == 92);
    REQUIRE_FALSE(scenery.touch({-22.1875f, 17.5f, 108.953125f}, 1).empty());
    scenery.update(0.051f);
    FallingScenery::LocalSnapshot state;
    scenery.saveLocal(state);
    const auto uploads = device.texturesCreated;
    scenery.update(0.137f);
    scenery.setPlayerCount(0);
    REQUIRE(scenery.restoreLocal(state));
    CHECK(holds(scenery, state));
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE("native falling scenery rewinds start cues half-frame motion and retirement",
          "[local-rewind][falling-local-rewind]") {
    Fixture reference("falling-native-reference");
    Fixture corrected("falling-native-corrected");
    LocalStateHistory<FallingScenery::LocalSnapshot> history;
    std::array<FallingScenery::LocalSnapshot, 8> expected;
    FallingScenery::LocalSnapshot observed;
    const auto* piece = &corrected.scenery.piece(0);
    const auto* model = &piece->model;
    const auto uploads = corrected.device.texturesCreated;
    s32 tick = 0;
    for (s32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        REQUIRE(history.capture(static_cast<u64>(tick), [&](auto& saved) {
            corrected.scenery.saveLocal(saved);
            return true;
        }));
        const s32 distance = 1 + window % 8;
        const auto* saved = history.find(static_cast<u64>(tick));
        REQUIRE(saved);
        for (s32 offset = 0; offset < distance; ++offset) {
            corrected.step(tick + offset, 1.0f / 60);
            corrected.scenery.saveLocal(expected[static_cast<usize>(offset)]);
        }
        REQUIRE(corrected.scenery.restoreLocal(*saved));
        std::vector<std::vector<FallingCue>> cues;
        for (s32 offset = 0; offset < distance; ++offset) {
            cues.push_back(reference.step(tick + offset, 1.0f / 60));
            corrected.scenery.setPlayerCount(4);
            corrected.scenery.touch({0, 0, 0}, 100);
            corrected.scenery.shoot({40, 0, 0}, 100);
            corrected.scenery.update(0.13f);
        }
        REQUIRE(corrected.scenery.restoreLocal(*saved));
        CHECK(holds(corrected.scenery, *saved));
        for (s32 offset = 0; offset < distance; ++offset) {
            checkCues(corrected.step(tick + offset, 1.0f / 60), cues[static_cast<usize>(offset)]);
            corrected.scenery.saveLocal(observed);
            CHECK(corrected.scenery.equalLocal(observed, expected[static_cast<usize>(offset)]));
        }
        CHECK(corrected.digest() == reference.digest());
        CHECK(&corrected.scenery.piece(0) == piece);
        CHECK(&corrected.scenery.piece(0).model == model);
        for (const f32 alpha : {-1.0f, 0.0f, 0.5f, 1.0f}) {
            reference.device.draws.clear();
            corrected.device.draws.clear();
            reference.scenery.draw(reference.device, Mat4{1}, {}, alpha);
            corrected.scenery.draw(corrected.device, Mat4{1}, {}, alpha);
            REQUIRE(corrected.device.draws.size() == reference.device.draws.size());
            for (usize draw = 0; draw < reference.device.draws.size(); ++draw) {
                CHECK(corrected.device.draws[draw].vertices ==
                      reference.device.draws[draw].vertices);
            }
        }
        tick += distance;
    }
    CHECK(corrected.device.texturesCreated == uploads);
    FallingScenery::LocalSnapshot saved;
    corrected.scenery.saveLocal(saved);
    CHECK_FALSE(reference.scenery.restoreLocal(saved));
    CHECK_FALSE(reference.scenery.equalLocal(saved, saved));
    corrected.scenery.clear();
    CHECK_FALSE(corrected.scenery.restoreLocal(saved));
    CHECK_FALSE(corrected.scenery.equalLocal(saved, saved));
    corrected.scenery.bind(corrected.device, corrected.layout, corrected.models, corrected.textures,
                           "D2");
    CHECK_FALSE(corrected.scenery.restoreLocal(saved));
}

TEST_CASE("native falling equality retains subframe time without a changed piece position",
          "[local-rewind][falling-local-rewind]") {
    Fixture fixture("falling-native-hidden-clock");
    FallingScenery::LocalSnapshot before;
    FallingScenery::LocalSnapshot after;
    FallingScenery::LocalSnapshot restored;
    fixture.scenery.saveLocal(before);
    const auto position = fixture.scenery.piece(0).motion.position;
    fixture.scenery.update(1.0f / 240);
    fixture.scenery.saveLocal(after);
    CHECK(fixture.scenery.piece(0).motion.position == position);
    CHECK_FALSE(fixture.scenery.equalLocal(before, after));
    REQUIRE(fixture.scenery.restoreLocal(before));
    fixture.scenery.saveLocal(restored);
    CHECK(fixture.scenery.equalLocal(before, restored));
}
} // namespace
