#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/io/Sha256.h"

#include "TestSupport.h"
#include "expansions/n64/audio/LegendsAudioMix.h"
#include "expansions/n64/audio/LegendsSounds.h"
#include "expansions/n64/audio/N64LegendsStream.h"
#include "expansions/n64/scenes/LegendsN64Music.h"

TEST_CASE("N64 level music uses native routing instead of filename inference",
          "[legends][audio][music]") {
    using gdl::legendsN64SongForLevel;
    CHECK(legendsN64SongForLevel("mount1") == 0);
    CHECK(legendsN64SongForLevel("castle6") == 7);
    CHECK(legendsN64SongForLevel("ice2") == 7);
    CHECK(legendsN64SongForLevel("ice5") == 4);
    CHECK(legendsN64SongForLevel("ice6") == 4);
    CHECK(legendsN64SongForLevel("town4") == 18);
    CHECK(legendsN64SongForLevel("boss1") == 28);
    CHECK(legendsN64SongForLevel("boss2") == 21);
    CHECK(legendsN64SongForLevel("tower2") == 17);
    CHECK(legendsN64SongForLevel("TOWER2") == 17);
    CHECK(legendsN64SongForLevel("MoUnT1") == 0);
    CHECK(legendsN64SongForLevel("ICE2") == 7);
    CHECK(legendsN64SongForLevel("secret2") == 37);
    CHECK(legendsN64SongForLevel("secret6") == 38);
    CHECK_FALSE(legendsN64SongForLevel("secret1"));
    CHECK_FALSE(legendsN64SongForLevel("_test1"));
    CHECK_FALSE(legendsN64SongForLevel("unknown"));
    CHECK_FALSE(legendsN64SongForLevel("../mount1"));
}

TEST_CASE("Every installed N64 map retains its executable's authored music route",
          "[legends][audio][music][native-map-music][assets]") {
    using namespace gdl;
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("Native N64 program and maps are not installed");
    }
    const auto program = readFile(root / "_program/game.bin");
    Sha256 hash;
    hash.update(program);
    REQUIRE(hash.hex() == "0b60632eb50c7e77957a5c28106b4f353ba69dccbe0b37f1f68d37a36fd6e585");
    const auto text = [&](u32 address) {
        REQUIRE(address >= 0xe0000400);
        const usize offset = address - 0xe0000400;
        REQUIRE(offset < program.size());
        const auto first = program.begin() + static_cast<std::ptrdiff_t>(offset);
        const auto last = std::find(first, program.end(), 0);
        REQUIRE(last != program.end());
        return std::string(first, last);
    };
    std::array<std::string, 16> realms;
    for (usize offset = 0x6c360 - 0x400; readU32BE(program, offset) != 16; offset += 24) {
        const u32 identity = readU32BE(program, offset);
        REQUIRE(identity < realms.size());
        realms[identity] = text(readU32BE(program, offset + 8));
    }
    usize maps = 0;
    for (usize offset = 0x6c468 - 0x400; readU32BE(program, offset) != 16; offset += 40) {
        const u32 realm = readU32BE(program, offset);
        REQUIRE(realm < realms.size());
        const auto name = realms[realm] + text(readU32BE(program, offset + 12));
        CAPTURE(name);
        const u16 token = readU16BE(program, offset + 20);
        const auto expected =
            token == 0xffff ? std::optional<u16>{} : std::optional{static_cast<u16>(token & 0x3ff)};
        CHECK(legendsN64SongForLevel(name) == expected);
        if (std::filesystem::is_regular_file(root / "levels" / name / "worlds.wad")) {
            ++maps;
            REQUIRE(expected);
        }
    }
    CHECK(maps == 34);
}

TEST_CASE("Native boss map music remains audible with boss effects and pause resume",
          "[legends][audio][music][native-map-music][assets]") {
    using namespace gdl;
    using namespace gdl::game;
    if constexpr (!GDL_LEGENDS_AUDIO) {
        SKIP("Native MusyX audio is disabled");
    }
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "audio/gauntlet.proj")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    for (const char* name : {"mount6", "ice5", "town5", "castle5", "boss1", "boss2", "hell2"}) {
        CAPTURE(name);
        const auto song = legendsN64SongForLevel(name);
        REQUIRE(song);
        auto source = std::make_shared<N64LegendsStream>();
        REQUIRE(source->open(root / "audio", *song));
        AudioMixer mixer(48000);
        SoundPlayer music(mixer);
        SoundPlayer effects(mixer);
        LegendsSounds native;
        REQUIRE(native.openN64(root));
        const auto handle =
            music.playStream(legends_audio_mix::prepare(source, legends_audio_mix::Source::Music),
                             true, 1, SoundCategory::Music);
        REQUIRE(handle != kNoSound);
        std::array<f32, 1600> output{};
        for (usize interval = 0; interval < 9; ++interval) {
            // Repeated native effects must not invalidate another engine's music
            // ID database or stop the separate Music-category source.
            REQUIRE(native.play(effects, 0x33) != kNoSound);
            f64 energy = 0;
            bool finite = true;
            for (usize tick = 0; tick < 600; ++tick) {
                music.update();
                effects.update();
                mixer.mix(output);
                if (tick >= 300) { // Measure music after the one-shot effect ends.
                    for (const f32 sample : output) {
                        finite = finite && std::isfinite(sample);
                        energy += static_cast<f64>(sample) * sample;
                    }
                }
            }
            CAPTURE(interval, energy);
            CHECK(finite);
            CHECK(energy > .001);
            REQUIRE(music.isPlaying(handle));
            if (interval == 3) {
                music.setCategoryPaused(SoundCategory::Music, true);
                mixer.mix(output);
                music.update();
                mixer.mix(output);
                CHECK(std::ranges::all_of(output, [](f32 value) { return value == 0; }));
                CHECK(music.isPlaying(handle));
                music.setCategoryPaused(SoundCategory::Music, false);
            }
        }
        music.stopAll();
        effects.stopAll();
    }
}
