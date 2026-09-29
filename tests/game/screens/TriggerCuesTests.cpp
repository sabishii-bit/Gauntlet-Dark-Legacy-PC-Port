#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/TriggerCues.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the castle's and mines' turntables sound their turning and their stop",
          "[game][screens][trigger-cues]") {
    CHECK(TriggerCues::rotatorSoundsOf(1).turning == "S_ROCKROTATE");
    CHECK(TriggerCues::rotatorSoundsOf(1).stopping == "S_ROCKSTOP");
    CHECK(TriggerCues::rotatorSoundsOf(9).turning == "S_METLROTATE");
    CHECK(TriggerCues::rotatorSoundsOf(9).stopping == "S_METLROTATESTO");
    CHECK(TriggerCues::rotatorSoundsOf(0).turning.empty());
    CHECK(TriggerCues::rotatorSoundsOf(2).stopping.empty());
}

TEST_CASE("a party refused by a crystal gate is told what it wants, once a frame",
          "[game][screens][trigger-cues][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("LEVELS/LEVELL1/world.json").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    const LevelTriggers& triggers = world.triggers();
    const LevelTrigger* gate = nullptr;
    for (usize i = 0; i < triggers.size() && gate == nullptr; ++i) {
        if (triggers.trigger(i).needsCrystals() && !triggers.trigger(i).chained) {
            gate = &triggers.trigger(i);
        }
    }
    REQUIRE(gate != nullptr);
    const s32 realm = gate->id;
    const std::vector<TriggerVisitor> visitors{TriggerVisitor{.position = gate->spot, .party = 0}};
    world.startTriggers({});
    world.updateTriggers(1.0f / 30.0f, visitors);
    LevelSoundscape audio;
    SwitchCutscene cutscene;
    std::vector<std::pair<std::string, usize>> opened;
    TriggerCues cues;
    cues.handle(world, audio, nullptr, cutscene,
                {.help = [](s32, usize) { return false; },
                 .openMessage =
                     [&](std::string_view name, usize page) {
                         opened.emplace_back(std::string(name), page);
                         return true;
                     }});
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].first == TriggerCues::kNeedCrystals);
    CHECK(opened[0].second == static_cast<usize>(realm));
    // Reported once: the next frame's handling finds nothing new.
    cues.handle(world, audio, nullptr, cutscene, {});
    CHECK(opened.size() == 1);
}

} // namespace
