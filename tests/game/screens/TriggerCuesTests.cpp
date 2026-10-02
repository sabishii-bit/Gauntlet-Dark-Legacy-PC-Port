#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/SoundSet.h"
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
                     },
                 .shake = {},
                 .helpAt = {}});
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].first == TriggerCues::kNeedCrystals);
    CHECK(opened[0].second == static_cast<usize>(realm));
    // Reported once: the next frame's handling finds nothing new.
    cues.handle(world, audio, nullptr, cutscene, {});
    CHECK(opened.size() == 1);
}

TEST_CASE("Tower rear crossings keep their authored cue and upper drawbridges stay silent",
          "[game][screens][trigger-cues][unpacked]") {
    const std::string_view targetName =
        GENERATE("L1CROSSING_EASY", "L1CROSSING_HARD", "L1DRAWB66", "L1DRAWB67");
    const bool crossing = targetName.starts_with("L1CROSSING");
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELL1/world.json").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    const LevelTrigger* marker = nullptr;
    for (usize i = 0; i < world.triggers().size(); ++i) {
        const auto& trigger = world.triggers().trigger(i);
        if (trigger.target >= 0 &&
            world.layout().objects()[static_cast<usize>(trigger.target)].name == targetName) {
            REQUIRE(trigger.sound == (crossing ? 0 : -1));
            if (marker == nullptr) {
                marker = &trigger;
            }
        }
    }
    REQUIRE(marker != nullptr);
    const s32 target = marker->target;
    world.startTriggers({});
    world.takeTriggerOpenings();
    for (s32 tick = 0; tick < 5; ++tick) {
        world.updateTriggers(1.0f / 60.0f, {});
        world.update(1.0f / 60.0f);
        for (const auto& event : world.takeTriggerOpenings()) {
            CHECK(event.target != target); // no sound from an untouched marker
        }
    }
    const std::vector<TriggerVisitor> visitors{{.position = marker->spot}};
    s32 starts = 0;
    for (s32 tick = 0; tick < 120; ++tick) {
        world.updateTriggers(1.0f / 60.0f, visitors);
        world.update(1.0f / 60.0f);
        for (const auto& event : world.takeTriggerOpenings()) {
            if (event.target == target) {
                ++starts;
                CHECK_FALSE(event.atOnce);
                CHECK(event.sound == (crossing ? 0 : -1));
                CHECK(event.subtype == (crossing ? 24 : 20));
            }
        }
    }
    CHECK(starts == 1); // holding contact cannot restart the sound each frame
    SoundSet bank;
    REQUIRE(bank.load(root / "audio/TOWAMB"));
    const auto opening = bank.find("S_ELVMETL");
    const auto stopping = bank.find("S_ELVMETSTPL");
    REQUIRE(opening.has_value());
    REQUIRE(stopping.has_value());
    // Retail's MET slot deliberately shares the field recordings with the crystal gates.
    REQUIRE(bank.entry(*opening).sequence.size() == 1);
    CHECK(bank.entry(*opening).sequence[0].sample == 9);   // ffield
    CHECK(bank.entry(*stopping).sequence[0].sample == 10); // ffieldn
}

} // namespace
