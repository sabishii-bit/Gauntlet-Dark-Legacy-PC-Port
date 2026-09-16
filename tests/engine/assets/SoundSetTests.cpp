#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "formats/WavWriter.h"

namespace {

using namespace gdl;

std::filesystem::path sampleSet(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "samples");
    const std::array<s16, 4> kBlip{0, 16384, -16384, 0};
    writeFile(dir / "samples/000.wav", formats::encodeWav(kBlip, 12000, 1));
    writeFile(dir / "samples/001.wav", formats::encodeWav(kBlip, 24000, 1));
    writeTextFile(dir / "sounds.json", R"({
  "bank": "TEST",
  "sounds": [
    {"index": 0, "name": "S_BLIP", "id": 0, "duration": 0.1, "volume": 127, "duck": 0,
     "priority": 0, "sequence": [{"sample": 0, "loopStart": false, "loopBack": false}]},
    {"index": 1, "name": "S_MUSIC", "id": 1, "duration": -1.0, "volume": 63,
     "sequence": [{"sample": 1, "loopStart": true, "loopBack": false},
                  {"sample": 0, "loopStart": false, "loopBack": true}]}
  ],
  "samples": [
    {"index": 0, "name": "blip", "file": "samples/000.wav", "sampleRate": 12000, "frames": 4},
    {"index": 1, "name": "tune", "file": "samples/001.wav", "sampleRate": 24000, "frames": 4}
  ]
})");
    return dir;
}

TEST_CASE("a sound set resolves names to sequences of decoded clips", "[assets][sound]") {
    SoundSet set;
    REQUIRE(set.load(sampleSet("sound-set")));
    REQUIRE(set.size() == 2);
    REQUIRE(set.find("S_MUSIC") == 1U);
    REQUIRE_FALSE(set.find("S_NOTHING").has_value());
    REQUIRE(set.entry(1).duration < 0.0f);
    REQUIRE(set.entry(1).sequence.size() == 2);

    const SoundSequence music = set.sequence(1);
    REQUIRE(music.steps.size() == 2);
    REQUIRE(music.loops());
    REQUIRE(music.steps[0].clip->sampleRate == 24000);
    REQUIRE(music.steps[1].clip->sampleRate == 12000);
    REQUIRE(music.steps[1].clip->samples[1] == 0.5f);
    REQUIRE(music.volume == 63.0f / 127.0f);

    const SoundSequence blip = set.sequence(0);
    REQUIRE_FALSE(blip.loops());
    REQUIRE(&set.sample(0) == blip.steps[0].clip);
}

TEST_CASE("a missing manifest or wave file is reported", "[assets][sound]") {
    SoundSet set;
    REQUIRE_FALSE(set.load(test::scratchDirectory("sound-set-empty")));
    const auto dir = sampleSet("sound-set-missing");
    std::filesystem::remove(dir / "samples/001.wav");
    REQUIRE(set.load(dir));
    REQUIRE_THROWS_AS(set.sample(1), FileError);
}

TEST_CASE("the unpacked common bank names the menu sounds", "[assets][sound][unpacked]") {
    const auto dir = test::unpackedOrSkip("audio/COMMON/sounds.json").parent_path();
    SoundSet set;
    REQUIRE(set.load(dir));
    REQUIRE(set.size() == 105);
    REQUIRE(set.find("S_OPTMENUMOVVRT") == 13U);
    const SoundSequence move = set.sequence(13);
    REQUIRE(move.steps.size() == 1);
    REQUIRE(move.steps[0].clip->frames() > 100);
}

} // namespace
