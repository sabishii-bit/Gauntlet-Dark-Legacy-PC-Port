#include <array>
#include <cmath>
#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/audio/MusicDeclicker.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"

namespace {

using namespace gdl;

std::filesystem::path sampleSet(std::string_view name) {
    const auto path = test::scratchDirectory(name) / "TEST";
    const std::array<test::NativeSoundSample, 2> samples{{
        {12000, {0, 16384, -16384, 0}},
        {24000, {0, 16384, -16384, 0}},
    }};
    test::writeNativeSoundBank(path, R"({"sounds":[
        {"name":"S_BLIP","id":7,"duration":0.1,"volume":127,
         "sequence":[{"sample":0}]},
        {"name":"S_MUSIC","id":8,"duration":-1,"volume":63,
         "sequence":[{"sample":1,"loopStart":true},{"sample":0,"loopBack":true}]}
    ]})",
                               samples);
    return path;
}

TEST_CASE("a native sound set resolves names to sequences of decoded clips", "[sound]") {
    SoundSet set;
    const auto path = sampleSet("sound-set");
    REQUIRE(set.load(path));
    REQUIRE(set.size() == 2);
    REQUIRE(set.find("S_MUSIC") == 1U);
    REQUIRE_FALSE(set.find("S_NOTHING").has_value());
    REQUIRE(set.entry(1).id == 8);
    REQUIRE(set.entry(1).duration < 0.0f);
    REQUIRE(set.entry(1).sequence.size() == 2);

    const SoundSequence music = set.sequence(1);
    REQUIRE(music.steps.size() == 2);
    REQUIRE(music.loops());
    REQUIRE(music.steps[0].clip->sampleRate == 24000);
    REQUIRE(music.steps[1].clip->sampleRate == 12000);
    REQUIRE(music.steps[1].clip->frames() == 4);
    REQUIRE(music.steps[1].clip->samples[1] == 0.5f);
    REQUIRE(music.steps[1].clip->samples[2] == -0.5f);
    REQUIRE(music.volume == 63.0f / 127.0f);
    REQUIRE(music.gainCurve == SoundGainCurve::Dcs);

    const SoundSequence blip = set.sequence(0);
    REQUIRE_FALSE(blip.loops());
    REQUIRE(&set.sample(0) == blip.steps[0].clip);
    REQUIRE(set.load(path.parent_path() / "test.vbk"));
    REQUIRE(set.size() == 2);
}

TEST_CASE("missing and malformed native banks clear the previous set", "[sound]") {
    SoundSet set;
    const auto path = sampleSet("sound-set-missing");
    REQUIRE(set.load(path));
    SECTION("missing bank") {
        std::filesystem::remove(path.parent_path() / "TEST.VBK");
    }
    SECTION("malformed bank") {
        writeTextFile(path.parent_path() / "TEST.VBK", "invalid native bank");
    }
    SECTION("malformed directory") {
        writeTextFile(path.parent_path() / "AUDATPS2.ROM", "invalid directory");
    }
    REQUIRE_FALSE(set.load(path));
    REQUIRE_FALSE(set.loaded());
    REQUIRE_FALSE(set.find("S_BLIP"));
}

TEST_CASE("sound sets do not load an exported manifest", "[sound]") {
    const auto path = test::scratchDirectory("sound-set-export");
    writeTextFile(path / "sounds.json", R"({"sounds":[{"name":"OLD"}],"samples":[]})");
    SoundSet set;
    REQUIRE_FALSE(set.load(path));
}

TEST_CASE("native fixture directory keeps adjacent banks and replaces their names", "[sound]") {
    const auto path = sampleSet("sound-set-adjacent");
    const std::array<test::NativeSoundSample, 1> samples{{{8000, {8192}}}};
    test::writeNativeSoundBank(path.parent_path() / "OTHER",
                               R"({"sounds":[{"name":"OTHER","sequence":[{"sample":0}]}]})",
                               samples);
    test::writeNativeSoundBank(path, R"({"sounds":[{"name":"NEW","sequence":[{"sample":0}]}]})",
                               samples);
    SoundSet set;
    REQUIRE(set.load(path));
    REQUIRE(set.size() == 1);
    REQUIRE(set.find("NEW"));
    REQUIRE_FALSE(set.find("S_BLIP"));
    REQUIRE(set.load(path.parent_path() / "OTHER"));
    REQUIRE(set.find("OTHER"));
}

TEST_CASE("unnamed native banks retain numbered calls and PCM-derived durations", "[sound]") {
    const auto path = sampleSet("sound-set-unnamed");
    std::filesystem::remove(path.parent_path() / "AUDATPS2.ROM");
    SoundSet set;
    REQUIRE(set.load(path));
    REQUIRE(set.find("TEST_00") == 0U);
    REQUIRE(set.find("TEST_01") == 1U);
    REQUIRE(set.entry(0).duration == 4.0f / 12000.0f);
    REQUIRE(set.entry(1).duration == -1.0f);
}

TEST_CASE("the native common bank names the menu sounds", "[assets][sound]") {
    const auto path = test::assetOrSkip("AUDIO/COMMON.VBK");
    SoundSet set;
    REQUIRE(set.load(path));
    REQUIRE(set.size() == 105);
    REQUIRE(set.find("S_OPTMENUMOVVRT") == 13U);
    const SoundSequence move = set.sequence(13);
    REQUIRE(move.steps.size() == 1);
    REQUIRE(move.steps[0].clip->frames() > 100);
}

TEST_CASE("native bank restoration repairs clicks once without moving sequence boundaries",
          "[sound][declick]") {
    const auto path = test::scratchDirectory("sound-set-restoration") / "TEST";
    test::NativeSoundSample sample;
    sample.sampleRate = 24000;
    sample.samples.assign(12000, 4096);
    sample.samples[4000] = -24576;
    const std::array samples{sample};
    test::writeNativeSoundBank(path, R"({"sounds":[
        {"name":"LOOP","duration":-1,"sequence":[{"sample":0,"loopStart":true,"loopBack":true}]}
    ]})",
                               samples);
    SoundSet raw;
    SoundSet restored;
    REQUIRE(raw.load(path, SoundSet::Restoration::Disabled));
    REQUIRE(restored.load(path));
    const auto original = raw.sequence(0);
    const auto clean = restored.sequence(0);
    REQUIRE(clean.steps.size() == 1);
    CHECK(clean.loops());
    CHECK(clean.steps[0].loopStart);
    CHECK(clean.steps[0].loopBack);
    CHECK(clean.steps[0].clip->frames() == original.steps[0].clip->frames());
    CHECK(clean.steps[0].clip->sampleRate == original.steps[0].clip->sampleRate);
    CHECK(std::abs(clean.steps[0].clip->samples[4000] - 0.125f) < 0.01f);
    const auto cached = clean.steps[0].clip->samples;
    CHECK(restored.sample(0).samples == cached);
    CHECK(&restored.sample(0) == clean.steps[0].clip);
    REQUIRE(restored.load(path, SoundSet::Restoration::Disabled));
    CHECK(restored.sample(0).samples == original.steps[0].clip->samples);
}

TEST_CASE("native voice and effect clips use the same restoration as streamed music",
          "[assets][sound][declick]") {
    const auto audio = test::assetOrSkip("AUDIO/AUDATPS2.ROM").parent_path();
    for (const auto* bank : {"VOICE1", "MAP_J", "DREAM", "COMMON"}) {
        CAPTURE(bank);
        SoundSet raw;
        SoundSet restored;
        REQUIRE(raw.load(audio / bank, SoundSet::Restoration::Disabled));
        REQUIRE(restored.load(audio / bank));
        REQUIRE(raw.size() == restored.size());
        u32 call = 0;
        if (std::string_view(bank) == "MAP_J") {
            REQUIRE(raw.find("S_J1NAME"));
            call = *raw.find("S_J1NAME");
        } else if (std::string_view(bank) == "DREAM") {
            REQUIRE(raw.find("S_JGRUN1DIECLOS"));
            call = *raw.find("S_JGRUN1DIECLOS");
        }
        CAPTURE(raw.entry(call).name);
        REQUIRE_FALSE(raw.entry(call).sequence.empty());
        const u32 index = raw.entry(call).sequence[0].sample;
        const auto& original = raw.sample(index);
        MusicDeclicker filter;
        std::vector<f32> impulses;
        filter.reset(original.sampleRate, original.channels);
        filter.feed(original.samples, impulses);
        filter.finish(impulses);
        std::vector<f32> expected;
        filter.reset(original.sampleRate, original.channels, MusicDeclicker::Pass::ShortBursts);
        filter.feed(impulses, expected);
        filter.finish(expected);
        const auto& actual = restored.sample(index);
        CHECK(actual.samples == expected);
        CHECK(actual.frames() == original.frames());
    }
}

} // namespace
