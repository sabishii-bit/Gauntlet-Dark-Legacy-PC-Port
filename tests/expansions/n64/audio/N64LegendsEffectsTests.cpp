#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <amuse/AudioGroupPool.hpp>
#include <amuse/SoundMacroState.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/audio/N64LegendsEffects.h"
#include "expansions/n64/audio/N64LegendsStream.h"

namespace {
using namespace gdl;

std::filesystem::path nativeAudio() {
    return test::dataDirectory().parent_path() / "assets/n64/extracted/audio";
}

std::vector<f32> render(StreamSource& source, usize frames, usize chunk) {
    std::vector<f32> result;
    while (result.size() / 2 < frames) {
        const usize before = result.size();
        if (!source.read(result, std::min(chunk, frames - result.size() / 2))) {
            break;
        }
        REQUIRE(result.size() > before);
    }
    return result;
}

bool audible(std::span<const f32> samples) {
    return std::ranges::any_of(samples, [](f32 value) { return std::abs(value) > .0001f; });
}
} // namespace

TEST_CASE("N64 macro decoding preserves REV_LEVEL and rejects unknown commands",
          "[legends][audio][native-effects]") {
    // Native pool words are big endian, with the opcode in the low byte.
    const std::array<u8, 8> command{0, 127, 64, 0x34, 0, 0, 0, 0};
    athena::io::MemoryReader reader(command.data(), command.size());
    amuse::SoundMacro macro;
    macro.readCmds<std::endian::big>(reader, 8);
    REQUIRE(macro.m_cmds.size() == 1);
    REQUIRE(macro.getCmd(0).Isa() == amuse::SoundMacro::CmdOp::ReverbLevel);
    const auto& reverb = dynamic_cast<const amuse::SoundMacro::CmdReverbLevel&>(macro.getCmd(0));
    CHECK(reverb.scale == 64);
    CHECK(reverb.add == 127);
    amuse::SoundMacroState state;
    state.initialize(0, &macro, 0);
    CHECK(state.m_reverbScale == 128);
    CHECK(state.m_reverbAdd == 0);
    CHECK(state.applyReverbLevel(.5f) == .5f);
    state.m_reverbScale = 64;
    CHECK(state.applyReverbLevel(1) == .5f);
    state.m_reverbAdd = 127;
    CHECK(state.applyReverbLevel(0) == 1);
    CHECK(state.applyReverbLevel(1) == 1);
    state.m_reverbScale = 0;
    state.m_reverbAdd = 0;
    CHECK(state.applyReverbLevel(1) == 0);
    state.m_reverbScale = 255;
    state.m_reverbAdd = 255;
    CHECK(state.applyReverbLevel(1) == 1);

    SECTION("unhandled opcodes fail before playback") {
        auto unknown = command;
        unknown[3] = 0xff;
        athena::io::MemoryReader bad(unknown.data(), unknown.size());
        amuse::SoundMacro unsupported;
        CHECK_THROWS_WITH(unsupported.readCmds<std::endian::big>(bad, 8),
                          "Unsupported MusyX macro opcode 255 at step 0");
        CHECK(unsupported.m_cmds.empty());
    }
    SECTION("partial command records are rejected") {
        athena::io::MemoryReader bad(command.data(), command.size());
        amuse::SoundMacro unsupported;
        CHECK_THROWS_WITH(unsupported.readCmds<std::endian::big>(bad, 7),
                          "MusyX macro has a truncated command");
    }
    SECTION("invalid program counters and null commands cannot crash the VM") {
        CHECK_THROWS_AS(macro.getCmd(-1), std::runtime_error);
        CHECK_THROWS_AS(macro.getCmd(1), std::runtime_error);
        CHECK_THROWS_AS(macro.assertPC(99), std::runtime_error);
        macro.m_cmds[0].reset();
        CHECK_THROWS_AS(macro.getCmd(0), std::runtime_error);
    }
}

TEST_CASE("N64 effects refuse missing and unaudited banks before creating sources",
          "[legends][audio][native-effects]") {
    N64LegendsEffects effects;
    CHECK_FALSE(effects.opened());
    CHECK(effects.cueIds().empty());
    CHECK_FALSE(effects.contains(0));
    CHECK_FALSE(effects.create(0));
    const auto directory = test::scratchDirectory("n64-effects-invalid");
    CHECK_FALSE(effects.open(directory));
    writeTextFile(directory / "gauntlet.proj", "Not a native bank");
    CHECK_FALSE(effects.open(directory));
    CHECK_FALSE(effects.opened());
}

TEST_CASE("N64 effects preserve group39 IDs and validate all native macros",
          "[legends][audio][native-effects][assets]") {
    const auto directory = nativeAudio();
    if (!std::filesystem::is_regular_file(directory / "gauntlet.proj")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    N64LegendsEffects effects;
    REQUIRE(effects.open(directory));
    REQUIRE(effects.cueIds().size() == 309);
    CHECK(effects.cueIds().front() == 0);
    CHECK(effects.cueIds().back() == 308);
    CHECK_FALSE(effects.contains(309));
    CHECK_FALSE(effects.contains(0xffff));
    CHECK_FALSE(effects.create(309));
    for (const f32 invalid : {-1.0f, 0.0f, 9.0f, std::numeric_limits<f32>::infinity(),
                              std::numeric_limits<f32>::quiet_NaN()}) {
        CHECK_FALSE(effects.create(0, invalid));
    }
    for (const u16 cue : effects.cueIds()) {
        CAPTURE(cue);
        auto source = effects.create(cue);
        REQUIRE(source);
        CHECK(source->desc().sampleRate == 32000);
        CHECK(source->desc().channels == 2);
        const auto samples = render(*source, 64000, 997);
        REQUIRE_FALSE(samples.empty());
        // Raw macro sums retain float headroom. Only the final stereo mix,
        // after native calibration and user gain, is limited to full scale.
        CHECK(std::ranges::all_of(samples, [](f32 value) { return std::isfinite(value); }));
        // The original obelisk activation uses native effect0x78, not a
        // SoundSet name or a sample index. It must produce actual PCM.
        if (cue == 0x78 || cue == 0x71 || cue == 10) {
            CHECK(audible(samples));
        }
    }
}

TEST_CASE("N64 effect sources rewind independently and outlive their bank facade",
          "[legends][audio][native-effects][assets]") {
    const auto directory = nativeAudio();
    if (!std::filesystem::is_regular_file(directory / "gauntlet.proj")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    N64LegendsEffects effects;
    REQUIRE(effects.open(directory));
    auto first = effects.create(0x78);
    auto second = effects.create(0x71);
    REQUIRE(first);
    REQUIRE(second);
    const auto expected = render(*first, usize{32000} * 8, 4096);
    CHECK(audible(expected));
    REQUIRE(expected.size() < usize{32000} * 8 * 2); // one-shot macro reaches EOF
    std::vector<f32> unchanged{.25f};
    CHECK_FALSE(first->read(unchanged, 160));
    CHECK(unchanged.size() == 1);
    for (const usize chunk : {1U, 997U, 8191U}) {
        first->rewind();
        const auto repeated = render(*first, usize{32000} * 8, chunk);
        CHECK(std::equal(expected.begin(), expected.end(), repeated.begin(), repeated.end()));
    }
    first->rewind();
    const auto other = render(*second, 10000, 160);
    CHECK(audible(other));
    CHECK_FALSE(effects.open(directory / "missing"));
    CHECK_FALSE(effects.opened());
    const auto surviving = render(*first, usize{32000} * 8, 997);
    CHECK(std::equal(expected.begin(), expected.end(), surviving.begin(), surviving.end()));
    second->rewind();
    const auto repeatedOther = render(*second, 10000, 997);
    CHECK(std::equal(other.begin(), other.end(), repeatedOther.begin(), repeatedOther.end()));
}

TEST_CASE("Native interactive fixture macros finish without a synthetic key off",
          "[legends][audio][native-effects][assets]") {
    const auto directory = nativeAudio();
    if (!std::filesystem::is_regular_file(directory / "gauntlet.proj")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    N64LegendsEffects effects;
    REQUIRE(effects.open(directory));
    // PTRAP+28: TPAD, the four doors, OBRD and explosive barrels. PCON+40:
    // barrel22/chest29; poison e0074eec:60. e0068118 plays activation once
    // and discards the returned voice, rather than keying off on the
    // animation's final frame. Preserve the complete authored macro.
    for (const u16 cue : std::array<u16, 10>{0x36, 0xba, 0xbb, 0x3a, 0x3b, 0x78, 22, 29, 60, 147}) {
        CAPTURE(cue);
        auto source = effects.create(cue);
        REQUIRE(source);
        std::vector<f32> chunk;
        usize frames = 0;
        bool heard = false;
        bool finished = false;
        while (frames < usize{32000} * 20) {
            chunk.clear();
            if (!source->read(chunk, 4096)) {
                finished = true;
                break;
            }
            frames += chunk.size() / 2;
            heard |= audible(chunk);
        }
        CHECK(heard);
        CHECK(finished);
    }
}

TEST_CASE("N64 effects use ordinary effect volume pause stop and mixer lifetimes",
          "[legends][audio][native-effects][assets]") {
    const auto directory = nativeAudio();
    if (!std::filesystem::is_regular_file(directory / "gauntlet.proj")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    N64LegendsEffects effects;
    REQUIRE(effects.open(directory));
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    const auto handle = output.playStream(effects.create(0x78), false, 1, SoundCategory::Effects);
    REQUIRE(handle != kNoSound);
    std::array<f32, 1600> samples{};
    bool heard = false;
    for (usize i = 0; i < 12; ++i) {
        mixer.mix(samples);
        heard |= audible(samples);
        output.update();
    }
    CHECK(heard);
    output.setCategoryPaused(SoundCategory::Effects, true);
    mixer.mix(samples);
    mixer.mix(samples);
    CHECK_FALSE(audible(samples));
    output.setCategoryPaused(SoundCategory::Effects, false);
    output.setCategoryVolume(SoundCategory::Effects, 0);
    mixer.mix(samples);
    mixer.mix(samples);
    CHECK_FALSE(audible(samples));
    output.setCategoryVolume(SoundCategory::Effects, 1);
    output.stop(handle);
    output.update();
    CHECK_FALSE(output.isPlaying(handle));
    mixer.mix(samples);
    mixer.mix(samples);
    CHECK_FALSE(audible(samples));
}

TEST_CASE("N64 effects share bank bytes without sharing music state or unbounded source counts",
          "[legends][audio][native-effects][assets]") {
    const auto directory = nativeAudio();
    if (!std::filesystem::is_regular_file(directory / "gauntlet.proj")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    N64LegendsEffects effects;
    REQUIRE(effects.open(directory));
    N64LegendsStream music;
    REQUIRE(music.open(directory, 17));
    const auto expected = render(music, 32000, 997);
    music.rewind();
    std::vector<std::shared_ptr<StreamSource>> sources;
    for (usize i = 0; i < 64; ++i) {
        sources.push_back(effects.create(0x78));
        REQUIRE(sources.back());
    }
    CHECK_FALSE(effects.create(0x78));
    sources.clear();
    REQUIRE(effects.create(0x78));
    const auto repeated = render(music, 32000, 997);
    CHECK(std::equal(expected.begin(), expected.end(), repeated.begin(), repeated.end()));
}

TEST_CASE("N64 effect bank authentication rejects changed samples and macros",
          "[legends][audio][native-effects][assets]") {
    const auto source = nativeAudio();
    if (!std::filesystem::is_regular_file(source / "gauntlet.proj")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    const auto directory = test::scratchDirectory("n64-effects-modified");
    constexpr std::array kNames{"gauntlet.proj", "gauntlet.pool", "gauntlet.sdir", "gauntlet.samp"};
    for (const auto* name : kNames) {
        writeFile(directory / name, readFile(source / name));
    }
    for (const auto* name : kNames) {
        CAPTURE(name);
        auto bytes = readFile(source / name);
        bytes[bytes.size() / 2] ^= 1;
        writeFile(directory / name, bytes);
        N64LegendsEffects effects;
        CHECK_FALSE(effects.open(directory));
        CHECK_FALSE(effects.create(0));
        writeFile(directory / name, readFile(source / name));
    }
}
