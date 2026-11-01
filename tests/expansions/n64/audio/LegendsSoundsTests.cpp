#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/audio/AudioMixer.h"
#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/audio/LegendsAudioMix.h"
#include "expansions/n64/audio/LegendsSoundTable.h"
#include "expansions/n64/audio/LegendsSounds.h"
#include "expansions/n64/audio/N64LegendsStream.h"
#include "expansions/n64/enemies/LegendsEnemySounds.h"
#include "expansions/shared/scenes/LegendsLevel.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path nativeRoot() {
    return test::dataDirectory().parent_path() / "assets/n64/extracted";
}

class MixSource final : public StreamSource {
public:
    AudioStreamDesc desc() const override { return {8000, 1}; }
    bool read(std::vector<f32>& output, usize frames) override {
        constexpr std::array<f32, 4> kPcm{.8f, -.8f, .25f, -.25f};
        const usize count = std::min(frames, kPcm.size() - m_cursor);
        output.insert(output.end(), kPcm.begin() + m_cursor, kPcm.begin() + m_cursor + count);
        m_cursor += count;
        // StreamSource signals whether this call produced a block, not whether
        // another block follows. SoundPlayer must receive the final samples.
        return count != 0;
    }
    void rewind() override { m_cursor = 0; }

private:
    usize m_cursor = 0;
};

class CalibrationSource final : public StreamSource {
public:
    AudioStreamDesc format{8000, 1};
    std::vector<f32> pcm;
    usize maximumRead = 4096;
    usize rewinds = 0;
    bool loop = false;
    AudioStreamDesc desc() const override { return format; }
    bool read(std::vector<f32>& output, usize frames) override {
        if (pcm.empty())
            return false;
        const usize count = std::min(frames, maximumRead);
        usize produced = 0;
        while (produced < count) {
            if (m_cursor == pcm.size()) {
                if (!loop)
                    break;
                m_cursor = 0;
            }
            output.push_back(pcm[m_cursor++]);
            ++produced;
        }
        return produced != 0;
    }
    void rewind() override {
        m_cursor = 0;
        ++rewinds;
    }

private:
    usize m_cursor = 0;
};
} // namespace

TEST_CASE("Native loudness calibration freezes one bounded chunk-independent gain",
          "[legends][audio][native-mix]") {
    using namespace legendsAudioMix;
    for (const usize chunk : {usize{1}, usize{73}, usize{4096}}) {
        auto source = std::make_shared<CalibrationSource>();
        source->maximumRead = chunk;
        source->pcm.resize(800, 0);
        for (usize i = 0; i < 400; ++i)
            source->pcm[i] = i % 2 == 0 ? .02f : -.02f;
        const auto measured = calibrate(*source, Source::Effect);
        REQUIRE(measured);
        CHECK(measured->complete);
        CHECK(measured->frames == 800);
        CHECK(measured->activeRms == Catch::Approx(.02));
        CHECK(measured->peak == .02f);
        CHECK(measured->gain == Catch::Approx(16));
        auto output = prepare(source, Source::Effect, measured);
        REQUIRE(output);
        const usize rewinds = source->rewinds;
        CHECK(prepare(output, Source::Effect) == output);
        CHECK(source->rewinds == rewinds);
        std::vector<f32> pcm;
        while (output->read(pcm, 119)) {
        }
        REQUIRE(pcm.size() == source->pcm.size());
        for (usize i = 0; i < pcm.size(); ++i)
            CHECK(pcm[i] == source->pcm[i] * measured->gain);
        output->rewind();
        std::vector<f32> replay;
        while (output->read(replay, 31)) {
        }
        CHECK(replay == pcm);
    }
    SECTION("loop analysis is bounded and starts playback at the original sample") {
        CalibrationSource source;
        source.pcm = {.01f, -.01f, .03f, -.03f};
        source.loop = true;
        const auto measured = calibrate(source, Source::Effect);
        REQUIRE(measured);
        CHECK_FALSE(measured->complete);
        CHECK(measured->frames == 8 * 8000);
        std::vector<f32> beginning;
        CHECK(source.read(beginning, 4));
        CHECK(beginning == source.pcm);
    }
    SECTION("silence and numerical noise are not amplified") {
        for (const f32 value : {0.f, .000001f}) {
            CalibrationSource source;
            source.pcm.assign(160, value);
            const auto measured = calibrate(source, Source::Effect);
            REQUIRE(measured);
            CHECK(measured->gain == 1);
            CHECK(measured->activeRms == 0);
        }
    }
    SECTION("isolated peaks retain float headroom without forcing excessive RMS gain") {
        CalibrationSource source;
        source.pcm.assign(800, .01f);
        source.pcm[400] = 1;
        const auto measured = calibrate(source, Source::Effect);
        REQUIRE(measured);
        CHECK(measured->gain == Catch::Approx(1.8));
    }
    SECTION("bad inputs cannot poison the shared mixer") {
        auto source = std::make_shared<CalibrationSource>();
        source->pcm = {std::numeric_limits<f32>::quiet_NaN()};
        CHECK_FALSE(prepare(source, Source::Effect));
        source->pcm = {.1f};
        source->format.channels = 3;
        CHECK_FALSE(prepare(source, Source::Effect));
        source->format = {8000, 1};
        CHECK_FALSE(prepare(source, Source::Effect, Calibration{.kind = Source::Music}));
        CHECK_FALSE(prepare(source, Source::Effect,
                            Calibration{.gain = std::numeric_limits<f32>::infinity()}));
        CHECK_FALSE(prepare(source, Source::Effect, Calibration{.gain = -1}));
    }
}

TEST_CASE("Every native macro can be calibrated safely without changing its raw decoder",
          "[legends][audio][native-mix-corpus][assets]") {
    if constexpr (!GDL_LEGENDS_AUDIO)
        SKIP("Native MusyX audio is disabled");
    if (!std::filesystem::is_regular_file(nativeRoot() / "_program/game.bin"))
        SKIP("Native N64 extraction is not installed");
    N64LegendsEffects bank;
    REQUIRE(bank.open(nativeRoot() / "audio"));
    const auto started = std::chrono::steady_clock::now();
    f64 maximumSeconds = 0;
    usize finite = 0;
    for (const u16 id : bank.cueIds()) {
        CAPTURE(id);
        auto source = bank.create(id);
        REQUIRE(source);
        const auto before = std::chrono::steady_clock::now();
        const auto measured = legendsAudioMix::calibrate(*source, legendsAudioMix::Source::Effect);
        maximumSeconds =
            std::max(maximumSeconds,
                     std::chrono::duration<f64>(std::chrono::steady_clock::now() - before).count());
        REQUIRE(measured);
        CHECK(std::isfinite(measured->gain));
        CHECK(measured->gain > 0);
        CHECK(measured->gain <= 16);
        CHECK(measured->peak * measured->gain <= 1.80001f);
        finite += measured->complete ? 1 : 0;
        auto raw = bank.create(id);
        REQUIRE(raw);
        auto normalized =
            legendsAudioMix::prepare(source, legendsAudioMix::Source::Effect, measured);
        REQUIRE(normalized);
        std::vector<f32> firstRaw, firstMixed;
        CHECK(raw->read(firstRaw, 320) == normalized->read(firstMixed, 320));
        REQUIRE(firstRaw.size() == firstMixed.size());
        for (usize i = 0; i < firstRaw.size(); ++i)
            CHECK(firstMixed[i] == firstRaw[i] * measured->gain);
    }
    const f64 total =
        std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
    WARN("Native calibration cues=" << bank.cueIds().size() << " complete=" << finite
                                    << " totalSeconds=" << total
                                    << " maximumSeconds=" << maximumSeconds);
    CHECK(bank.cueIds().size() == 309);
}

TEST_CASE("Native effect calibration is prewarmed safely for concurrent level loaders",
          "[legends][audio][native-mix-cache][assets]") {
    if constexpr (!GDL_LEGENDS_AUDIO)
        SKIP("Native MusyX audio is disabled");
    if (!std::filesystem::is_regular_file(nativeRoot() / "_program/game.bin"))
        SKIP("Native N64 extraction is not installed");
    const auto open = [] {
        auto sounds = std::make_unique<LegendsSounds>();
        return sounds->openN64(nativeRoot()) ? std::move(sounds) : nullptr;
    };
    const auto started = std::chrono::steady_clock::now();
    auto pendingFirst = std::async(std::launch::async, open);
    auto pendingSecond = std::async(std::launch::async, open);
    auto first = pendingFirst.get();
    auto second = pendingSecond.get();
    REQUIRE(first);
    REQUIRE(second);
    const f64 loadSeconds =
        std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
    AudioMixer firstMixer(48000), secondMixer(48000);
    SoundPlayer firstOutput(firstMixer), secondOutput(secondMixer);
    const auto beforePlayback = std::chrono::steady_clock::now();
    const auto firstHandle = first->play(firstOutput, 116);
    const f64 playbackSeconds =
        std::chrono::duration<f64>(std::chrono::steady_clock::now() - beforePlayback).count();
    REQUIRE(firstHandle != kNoSound);
    REQUIRE(second->play(secondOutput, 116) != kNoSound);
    std::array<f32, 1600> a{}, b{};
    bool audible = false;
    for (usize frame = 0; frame < 120; ++frame) {
        firstOutput.update();
        secondOutput.update();
        firstMixer.mix(a);
        secondMixer.mix(b);
        REQUIRE(a == b);
        audible = audible || std::ranges::any_of(a, [](f32 value) { return value != 0; });
    }
    CHECK(audible);
    // Reopening still authenticates the supplied assets; a warm process cache
    // never turns a missing or unaudited bank into a usable level sound route.
    CHECK_FALSE(second->openN64(test::scratchDirectory("native-mix-cache-missing") / "absent"));
    CHECK_FALSE(second->opened());
    CHECK(second->play(secondOutput, 116) == kNoSound);
    REQUIRE(second->openN64(nativeRoot()));
    REQUIRE(second->play(secondOutput, 116) != kNoSound);
    WARN("Native calibration loaderSeconds=" << loadSeconds
                                             << " firstPlaybackSeconds=" << playbackSeconds);
}

TEST_CASE("Native music and boss effects expose comparable mix measurements",
          "[legends][audio][native-mix-survey][assets]") {
    const auto root = nativeRoot();
    if constexpr (!GDL_LEGENDS_AUDIO)
        SKIP("Native MusyX audio is disabled");
    if (!std::filesystem::is_regular_file(root / "_program/game.bin"))
        SKIP("Native N64 audio extraction is not installed");
    const auto measure = [](StreamSource& source, usize seconds) {
        const usize samples = source.desc().sampleRate * seconds * source.desc().channels;
        std::vector<f32> pcm;
        while (pcm.size() < samples) {
            const usize request =
                std::min<usize>(4096, (samples - pcm.size()) / source.desc().channels);
            const usize before = pcm.size();
            if (!source.read(pcm, request))
                break;
            REQUIRE(pcm.size() > before);
        }
        REQUIRE_FALSE(pcm.empty());
        f64 energy = 0;
        f32 peak = 0;
        usize active = 0;
        bool finite = true;
        for (f32 value : pcm) {
            finite = finite && std::isfinite(value);
            energy += static_cast<f64>(value) * value;
            peak = std::max(peak, std::abs(value));
            active += std::abs(value) > .00001f ? 1 : 0;
        }
        const f64 rms = std::sqrt(energy / static_cast<f64>(pcm.size()));
        const f64 activeRms = active ? std::sqrt(energy / static_cast<f64>(active)) : 0;
        INFO("RMS=" << rms << " activeRMS=" << activeRms << " peak=" << peak);
        CHECK(finite);
        CHECK(rms > .000001);
    };
    for (u16 song : std::array<u16, 4>{0, 17, 21, 28}) {
        INFO("neutral music song=" << song);
        N64LegendsStream source;
        REQUIRE(source.open(root / "audio", song));
        measure(source, 30);
    }
    LegendsSounds routes;
    N64LegendsEffects bank;
    REQUIRE(routes.openN64(root));
    REQUIRE(bank.open(root / "audio"));
    const auto rows = decodeLegendsSoundTable(readFile(root / "_program/game.bin"));
    for (const auto& row : rows) {
        if (!row.name.starts_with("s_skorn"))
            continue;
        INFO("effect=" << row.name);
        const auto token = routes.namedToken(row.name);
        REQUIRE(token);
        const auto cue = routes.resolve(*token);
        REQUIRE(cue);
        auto source = bank.create(cue->effect, cue->pitch, cue->volume, cue->pan);
        REQUIRE(source);
        measure(*source, 4);
    }
}

TEST_CASE("Native enemy playback exposes final mixer levels at gameplay distances",
          "[legends][audio][native-enemy-mix-survey][assets]") {
    if constexpr (!GDL_LEGENDS_AUDIO)
        SKIP("Native MusyX audio is disabled");
    const auto root = nativeRoot();
    const auto directory = root / "levels/boss1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad"))
        SKIP("Native N64 extraction is not installed");
    const auto characters = test::assetOrSkip("PLAYERS").parent_path();
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    LegendsLevel level(device, map, std::move(adapter), directory, characters);
    const auto enemies = LegendsEnemySounds::decode(readFile(root / "_program/game.bin"),
                                                    readFile(root / "enemy.rom"));
    LegendsSounds routes;
    REQUIRE(routes.openN64(root));
    const auto measure = [](AudioMixer& mixer, SoundPlayer& output) {
        std::array<f32, 1600> block{};
        f64 energy = 0;
        f64 activeEnergy = 0;
        usize active = 0;
        f32 peak = 0;
        bool finite = true;
        // Four seconds include the complete short enemy cries and mixer tail.
        // Gate short stereo blocks, not samples: zero crossings are not silence.
        for (usize tick = 0; tick < 240; ++tick) {
            output.update();
            mixer.mix(block);
            for (usize at = 0; at < block.size(); at += 160) {
                f64 windowEnergy = 0;
                for (usize i = at; i < at + 160; ++i) {
                    finite = finite && std::isfinite(block[i]);
                    peak = std::max(peak, std::abs(block[i]));
                    windowEnergy += static_cast<f64>(block[i]) * block[i];
                }
                energy += windowEnergy;
                if (windowEnergy / 160 > .000001) {
                    activeEnergy += windowEnergy;
                    active += 160;
                }
            }
        }
        REQUIRE(finite);
        REQUIRE(active != 0);
        REQUIRE(peak <= AudioMixer::kCeiling);
        return std::array<f64, 3>{std::sqrt(energy / (240 * block.size())),
                                  std::sqrt(activeEnergy / static_cast<f64>(active)), peak};
    };
    // One actual species from each native voice family, with both health and
    // death routing. Exercise the real level facade rather than raw macro PCM.
    std::set<u32> measuredTokens;
    for (const s32 kind : {0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19}) {
        for (const bool killed : {false, true}) {
            const EnemyFeedback event{.kind = kind, .tier = 3, .killed = killed};
            const auto token = enemies.feedback(event, killed ? 0.f : 8.f);
            REQUIRE(token);
            if (!measuredTokens.insert(*token).second)
                continue;
            const auto cue = routes.resolve(*token);
            REQUIRE(cue);
            for (const f32 slider : {128.f / 255.f, 1.f}) {
                std::optional<std::array<f64, 3>> near;
                for (const f32 distance : {4.f, 30.f, 80.f}) {
                    AudioMixer mixer(48000);
                    SoundPlayer output(mixer);
                    output.setCategoryVolume(SoundCategory::Effects, slider);
                    REQUIRE(level.playEnemyFeedback(event, killed ? 0.f : 8.f));
                    // Boss1 has no authored ambient emitter here and has not
                    // begun its camera intro/speech. These are only combat cues.
                    level.playSounds(output, Vec3{distance, 40, 0});
                    const auto result = measure(mixer, output);
                    // The formerly quiet native families must now reach an
                    // audible range at ordinary settings, not merely be nonzero.
                    CHECK(result[1] > (slider < 1 ? .095 : .17));
                    WARN("N64 enemy kind=" << kind << " killed=" << killed << " token=" << *token
                                           << " nativeVolume=" << cue->volume
                                           << " slider=" << slider << " distance=" << distance
                                           << " rms=" << result[0] << " activeRms=" << result[1]
                                           << " peak=" << result[2]);
                    if (near)
                        CHECK(result == *near); // Native combat cues are global.
                    else
                        near = result;
                    level.stopSounds();
                }
            }
        }
    }
    // Same decoder-independent final mix and normal default slider for a
    // representative native GDL enemy bank; names come from that bank itself.
    SoundSet gc;
    REQUIRE(gc.load(test::assetOrSkip("AUDIO") / "mount"));
    for (usize i = 0; i < gc.size(); ++i) {
        const auto& entry = gc.entry(static_cast<u32>(i));
        if (!(entry.name.starts_with("S_TROL") && (entry.name.find("HIT") != std::string::npos ||
                                                   entry.name.find("DIE") != std::string::npos)))
            continue;
        for (const f32 slider : {128.f / 255.f, 1.f}) {
            AudioMixer mixer(48000);
            SoundPlayer output(mixer);
            output.setCategoryVolume(SoundCategory::Effects, slider);
            REQUIRE(output.play(gc.sequence(static_cast<u32>(i))) != kNoSound);
            const auto result = measure(mixer, output);
            WARN("GC enemy name=" << entry.name << " slider=" << slider << " rms=" << result[0]
                                  << " activeRms=" << result[1] << " peak=" << result[2]);
        }
    }
}

TEST_CASE("Native cue-name lookup preserves first full case-insensitive match without fallback",
          "[legends][audio][native-boss-audio]") {
    const std::array rows{LegendsSoundEntry{.name = "S_TEST"}, LegendsSoundEntry{.name = "s_test"},
                          LegendsSoundEntry{.name = "S_TEST_LONG"}};
    CHECK(legendsSoundToken(rows, "s_TeSt") == 0x10000U);
    CHECK(legendsSoundToken(rows, "S_TEST_LONG") == 0x10002U);
    CHECK_FALSE(legendsSoundToken(rows, "S_TEST_LON"));
    CHECK_FALSE(legendsSoundToken(rows, "S_TEST_EXTRA"));
    CHECK_FALSE(legendsSoundToken(rows, ""));
    CHECK_FALSE(legendsSoundToken({}, "S_TEST"));
    CHECK_THROWS_AS(decodeLegendsSoundTable({}), FormatError);
}

TEST_CASE("Native cue metadata remains available without the optional synthesis backend",
          "[legends][audio][native-boss-audio][assets]") {
    const auto path = nativeRoot() / "_program/game.bin";
    if (!std::filesystem::is_regular_file(path))
        SKIP("Native N64 program is not installed");
    auto program = readFile(path);
    const auto rows = decodeLegendsSoundTable(program);
    REQUIRE(rows.size() == 124);
    LegendsSounds sounds;
    if constexpr (GDL_LEGENDS_AUDIO)
        REQUIRE(sounds.openN64(nativeRoot()));
    struct Duplicate {
        std::string_view name;
        usize first;
        usize repeated;
        u16 effect;
    };
    // The US table repeats these six complete routes. e007a408 scans from row0,
    // so both strings resolve to the first row, not their current row identity.
    constexpr std::array duplicates{
        Duplicate{"s_genkill", 20, 78, 60}, Duplicate{"s_drgdeath", 21, 73, 47},
        Duplicate{"s_drgpain", 22, 74, 50}, Duplicate{"s_drgbreez", 24, 72, 46},
        Duplicate{"s_drgwing", 25, 77, 57}, Duplicate{"s_drgtalon", 27, 76, 52}};
    for (const auto& duplicate : duplicates) {
        for (const usize index : {duplicate.first, duplicate.repeated}) {
            CAPTURE(index, duplicate.name);
            const auto& row = rows[index];
            CHECK(row.name == duplicate.name);
            CHECK(row.token == 0xffff);
            CHECK(row.replacement == duplicate.effect);
            CHECK(row.volume == 255);
            CHECK(row.pan == 128);
            CHECK(row.pitch == 1);
        }
    }
    usize namedRows = 0;
    std::set<std::string> names;
    for (usize index = 0; index < rows.size(); ++index) {
        const auto& name = rows[index].name;
        if (name.empty())
            continue;
        CAPTURE(index, name);
        const auto token = legendsSoundToken(rows, name);
        usize expectedFirst = index;
        for (const auto& duplicate : duplicates) {
            if (index == duplicate.repeated)
                expectedFirst = duplicate.first;
        }
        REQUIRE(token == (0x10000U | static_cast<u32>(expectedFirst)));
        if constexpr (GDL_LEGENDS_AUDIO) {
            CHECK(sounds.namedToken(name) == token);
            CHECK(sounds.resolve(*token));
        }
        ++namedRows;
        names.insert(name);
    }
    CHECK(namedRows == 81);
    CHECK(names.size() == 75);
    program.back() ^= 1;
    CHECK_THROWS_AS(decodeLegendsSoundTable(program), FormatError);
}

TEST_CASE("Native mix compensation preserves source headroom cursor and append semantics",
          "[legends][audio][fixture-calibration]") {
    auto raw = std::make_shared<MixSource>();
    CHECK_FALSE(legendsAudioMix::prepare({}, legendsAudioMix::Source::Effect));
    CHECK_FALSE(legendsAudioMix::prepare(raw, static_cast<legendsAudioMix::Source>(99)));
    const legendsAudioMix::Calibration headroom{.kind = legendsAudioMix::Source::Effect,
                                                .gain = 1.5f};
    auto source = legendsAudioMix::prepare(raw, legendsAudioMix::Source::Effect, headroom);
    CHECK(legendsAudioMix::prepare(source, legendsAudioMix::Source::Effect) == source);
    CHECK_FALSE(legendsAudioMix::prepare(source, legendsAudioMix::Source::Speech));
    raw.reset(); // The wrapper retains the decoder independently of its caller.
    REQUIRE(source);
    CHECK(source->desc().sampleRate == 8000);
    CHECK(source->desc().channels == 1);
    std::vector<f32> pcm{42};
    CHECK_FALSE(source->read(pcm, 0));
    CHECK(pcm == std::vector<f32>{42});
    CHECK(source->read(pcm, 2));
    CHECK(pcm == std::vector<f32>{42, 1.2f, -1.2f}); // No premature clipping.
    CHECK(source->read(pcm, 99));
    CHECK(pcm == std::vector<f32>{42, 1.2f, -1.2f, .375f, -.375f});
    CHECK_FALSE(source->read(pcm, 1));
    source->rewind();
    std::vector<f32> replay;
    CHECK(source->read(replay, 4));
    CHECK(replay == std::vector<f32>{1.2f, -1.2f, .375f, -.375f});
    CHECK_FALSE(source->read(replay, 1));
}

TEST_CASE("All native PCM categories share one non-cumulative playback boundary",
          "[legends][audio][native-mix][fixture-calibration]") {
    using Source = legendsAudioMix::Source;
    constexpr std::array cases{std::pair{Source::Music, .30f}, std::pair{Source::Effect, .32f},
                               std::pair{Source::Speech, .36f}};
    for (const auto& [kind, target] : cases) {
        CAPTURE(kind);
        MixSource reference;
        const auto calibration = legendsAudioMix::calibrate(reference, kind);
        REQUIRE(calibration);
        REQUIRE(calibration->complete);
        CHECK(calibration->frames == 4);
        const f32 rms = std::sqrt((.8f * .8f + .25f * .25f) * .5f);
        CHECK(calibration->activeRms == Catch::Approx(rms));
        CHECK(calibration->gain == Catch::Approx(target / rms));
        const f32 gain = calibration->gain;
        auto source = legendsAudioMix::prepare(std::make_shared<MixSource>(), kind);
        REQUIRE(source);
        // Repeated adapter application must not multiply either the samples or
        // the cursor. This is also exercised after the source has started.
        CHECK(legendsAudioMix::prepare(source, kind) == source);
        std::vector<f32> pcm;
        REQUIRE(source->read(pcm, 1));
        CHECK(legendsAudioMix::prepare(source, kind) == source);
        CHECK(source->read(pcm, 3));
        CHECK(pcm == std::vector<f32>{.8f * gain, -.8f * gain, .25f * gain, -.25f * gain});
        source->rewind();
        std::vector<f32> repeated;
        CHECK(source->read(repeated, 4));
        CHECK(repeated == pcm);
        CHECK_FALSE(source->read(repeated, 1));
        // User silence and limiting remain properties of the common engine,
        // not clipped or normalized-away values in the native decoder.
        for (const f32 volume : {0.f, .125f, 1.f}) {
            for (const u32 rate : {8000U, 44100U, 48000U}) {
                CAPTURE(volume, rate);
                AudioMixer mixer(rate);
                SoundPlayer output(mixer);
                output.setCategoryVolume(SoundCategory::Effects, volume);
                source->rewind();
                REQUIRE(output.playStream(source, false, 1, SoundCategory::Effects) != kNoSound);
                std::array<f32, 64> actual{};
                mixer.mix(actual);
                CHECK(std::ranges::all_of(actual, [](f32 sample) {
                    return std::isfinite(sample) && std::abs(sample) <= AudioMixer::kCeiling;
                }));
                if (volume == 0)
                    CHECK(std::ranges::all_of(actual, [](f32 sample) { return sample == 0; }));
                else
                    CHECK(std::ranges::any_of(actual, [](f32 sample) { return sample != 0; }));
            }
        }
    }
}

TEST_CASE("Native Legends sound tokens retain authored aliases gain and pitch",
          "[legends][audio][native-effects][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    LegendsSounds sounds;
    REQUIRE(sounds.openN64(root));
    REQUIRE(sounds.opened());
    const auto obelisk = sounds.resolve(0x78);
    REQUIRE(obelisk);
    CHECK(obelisk->effect == 0x78);
    CHECK(obelisk->volume == 1);
    CHECK(obelisk->pitch == 1);
    const auto alias = sounds.resolve(0x1f8);
    REQUIRE(alias);
    CHECK(alias->effect == 0x51);
    CHECK(alias->pitch == .5f);
    const auto movement = sounds.resolve(0x1fe);
    REQUIRE(movement);
    CHECK(movement->effect == 5);
    CHECK(movement->volume == 1);
    CHECK(movement->pitch == 1);
    const auto movementEnd = sounds.resolve(0x1ff);
    REQUIRE(movementEnd);
    CHECK(movementEnd->effect == 8);
    CHECK(movementEnd->volume == 1);
    CHECK(movementEnd->pitch == 1);
    const auto indexed = sounds.resolve(0x10008);
    REQUIRE(indexed);
    CHECK(indexed->effect == 0x11a);
    CHECK(indexed->volume == Catch::Approx(89.0f / 127));
    CHECK(indexed->pitch == 1);
    CHECK_FALSE(sounds.resolve(0xffffffff));
    CHECK_FALSE(sounds.resolve(0xffff));
    CHECK_FALSE(sounds.resolve(309));
    CHECK_FALSE(sounds.resolve(0x1007c));
    CHECK_FALSE(sounds.resolve(0x11000));
    CHECK_FALSE(sounds.resolve(0x10068)); // narration alias, not effect43
    CHECK_FALSE(sounds.resolve(0x10011)); // streamed speech, not effect267
    CHECK_FALSE(sounds.resolve(0x2000));
    CHECK_FALSE(sounds.resolve(0x4000));
    CHECK_FALSE(sounds.resolve(0x100000));
}

TEST_CASE("Native Legends cue routing reaches the effects mixer without name substitutes",
          "[legends][audio][native-effects][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    LegendsSounds sounds;
    REQUIRE(sounds.openN64(root));
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    const auto handle = sounds.play(output, 0x78);
    REQUIRE(handle != kNoSound);
    std::array<f32, 1600> samples{};
    f64 energy = 0;
    for (usize i = 0; i < 120; ++i) {
        mixer.mix(samples);
        for (const auto value : samples) {
            energy += static_cast<f64>(value) * value;
        }
        output.update();
    }
    CHECK(energy > .001);
    CHECK(sounds.play(output, 0xffff) == kNoSound);
    CHECK(sounds.play(output, 0x78, 0) == kNoSound);
    CHECK(sounds.play(output, 0x78, std::numeric_limits<f32>::quiet_NaN()) == kNoSound);
    CHECK_FALSE(sounds.openN64(root / "missing"));
    CHECK_FALSE(sounds.opened());
    CHECK_FALSE(sounds.resolve(0x78));
}

TEST_CASE("Native Legends sound routing requires the verified original program",
          "[legends][audio][native-effects]") {
    const auto directory = test::scratchDirectory("legends-sound-routing-invalid");
    LegendsSounds sounds;
    CHECK_FALSE(sounds.opened());
    CHECK_FALSE(sounds.openN64(directory));
    std::filesystem::create_directories(directory / "_program");
    writeFile(directory / "_program/game.bin", std::vector<u8>(0x80000, 0));
    CHECK_FALSE(sounds.openN64(directory));
    CHECK_FALSE(sounds.opened());
    CHECK_FALSE(sounds.resolve(0));
}

TEST_CASE("Native boss cue names resolve the original sound table without counterpart aliases",
          "[legends][audio][native-boss-audio][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    LegendsSounds sounds;
    CHECK_FALSE(sounds.namedToken("S_DRGDEATH"));
    REQUIRE(sounds.openN64(root));
    const auto bytes = readFile(root / "_program/game.bin");
    std::vector<std::string> names;
    usize namedRows = 0;
    for (usize i = 0; i < 124; ++i) {
        const usize offset = 0x7ab4c - 0x400 + i * 20;
        const u32 address = readU32BE(bytes, offset + 8);
        if (address == 0) {
            continue;
        }
        ++namedRows;
        REQUIRE(address >= 0xe0000400);
        const usize nameOffset = address - 0xe0000400;
        REQUIRE(nameOffset < bytes.size());
        const auto begin = bytes.begin() + nameOffset;
        const auto end = std::find(begin, bytes.end(), 0);
        REQUIRE(end != bytes.end());
        const std::string name(begin, end);
        CAPTURE(name, i);
        const auto token = sounds.namedToken(name);
        REQUIRE(token);
        if (std::ranges::find(names, name) == names.end()) {
            CHECK(*token == (0x10000U | static_cast<u32>(i)));
            names.push_back(name);
        }
        const auto cue = sounds.resolve(*token);
        REQUIRE(cue);
        CHECK(cue->effect == readU16BE(bytes, offset + 2));
        CHECK(cue->volume == 1);
        CHECK(cue->pitch == 1);
    }
    CHECK(namedRows == 81);
    CHECK(names.size() == 75);
    CHECK(sounds.namedToken("S_DRGBREEZ") == 0x10018);
    CHECK(sounds.namedToken("S_SKORN9ATTCK9") == 0x10032);
    CHECK_FALSE(sounds.namedToken("S_SKORN2ATTCK9")); // Native spelling differs.
    CHECK_FALSE(sounds.namedToken("S_CHIMBITE"));     // A later-game effect is not an alias.
    CHECK(sounds.namedToken("s_drgdeath") == sounds.namedToken("S_DRGDEATH"));
    CHECK_FALSE(sounds.namedToken("S_DRG"));
    CHECK_FALSE(sounds.namedToken(""));
    CHECK_FALSE(sounds.openN64(root / "missing"));
    CHECK_FALSE(sounds.namedToken("S_DRGDEATH"));
}

TEST_CASE("Native fixture audio mix has measurable near-field output",
          "[legends][audio][native-effects][fixture-mix][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    const auto measure = [](AudioMixer& mixer, SoundPlayer& output) {
        std::array<f32, 1600> samples{};
        f64 energy = 0;
        f32 peak = 0;
        usize active = 0;
        bool finite = true;
        for (usize tick = 0; tick < 120; ++tick) {
            output.update();
            mixer.mix(samples);
            for (const f32 sample : samples) {
                finite = finite && std::isfinite(sample);
                peak = std::max(peak, std::abs(sample));
                energy += static_cast<f64>(sample) * sample;
                active += std::abs(sample) > .00001f ? 1 : 0;
            }
        }
        const f64 rms = std::sqrt(energy / static_cast<f64>(120 * samples.size()));
        const f64 activeRms = active == 0 ? 0 : std::sqrt(energy / static_cast<f64>(active));
        CAPTURE(rms, activeRms, peak, active);
        CHECK(finite);
        CHECK(rms > .000001);
        CHECK(peak <= AudioMixer::kCeiling);
    };
    LegendsSounds native;
    REQUIRE(native.openN64(root));
    for (const u32 token : {0x36U, 0x1feU, 0x1ffU, 0xbaU, 0xbbU, 0x78U, 22U, 29U, 60U}) {
        CAPTURE(token);
        AudioMixer mixer(48000);
        SoundPlayer output(mixer);
        REQUIRE(native.play(output, token) != kNoSound);
        measure(mixer, output);
    }
    // Comparable native GC fixture calls, not aliases for N64 identifiers.
    // These use their original bank gain and DCS curve through the same mixer.
    const auto directory = test::assetOrSkip("AUDIO");
    for (const auto& [bankName, name] :
         std::array{std::pair{"common", "S_CHEST"}, std::pair{"mount", "S_ELVSTONEB"},
                    std::pair{"mount", "S_ELVSTONESTPB"}, std::pair{"mount", "S_GATEB1"},
                    std::pair{"mount", "S_BARREL_WOODB"}}) {
        CAPTURE(bankName, name);
        SoundSet bank;
        REQUIRE(bank.load(directory / bankName));
        const auto id = bank.find(name);
        REQUIRE(id);
        AudioMixer mixer(48000);
        SoundPlayer output(mixer);
        REQUIRE(output.play(bank.sequence(*id)) != kNoSound);
        measure(mixer, output);
    }
}

TEST_CASE("Native fixtures ambience and boss cues share compensation before the effects slider",
          "[legends][audio][native-effects][fixture-calibration][assets]") {
    const auto root = nativeRoot();
    if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    LegendsSounds sounds;
    N64LegendsEffects effects;
    REQUIRE(sounds.openN64(root));
    REQUIRE(effects.open(root / "audio"));
    std::vector<u32> tokens{
        0x36U, 0x1feU, 0x1ffU, 0xbaU, 0xbbU,
        0x78U, 22U,    29U,    60U,   0x10008U}; // Authored sub-unity volume must remain intact.
    for (const auto name : {"S_SKORN1TAUNT", "S_SKORN1PROAR", "S_SKORN1DEATH", "S_SKORN2DEATH"}) {
        const auto token = sounds.namedToken(name);
        REQUIRE(token);
        tokens.push_back(*token);
    }
    for (const u32 token : tokens) {
        CAPTURE(token);
        const auto cue = sounds.resolve(token);
        REQUIRE(cue);
        auto source = effects.create(cue->effect, cue->pitch, cue->volume, cue->pan);
        REQUIRE(source);
        auto neutral = effects.create(cue->effect);
        REQUIRE(neutral);
        const auto calibration =
            legendsAudioMix::calibrate(*neutral, legendsAudioMix::Source::Effect);
        REQUIRE(calibration);
        std::vector<f32> decoded;
        // Some macros loop internally. Compare a bounded two-second interval,
        // including interpolation lookahead, without changing their lifetime.
        // A source may cap each read; one large request does not fill the window.
        const usize frames = static_cast<usize>(source->desc().sampleRate) * 2 + 16;
        const usize channels = source->desc().channels;
        REQUIRE(channels > 0);
        while (decoded.size() / channels < frames) {
            const usize before = decoded.size();
            const usize request = std::min<usize>(4096, frames - before / channels);
            if (!source->read(decoded, request)) {
                break;
            }
            REQUIRE(decoded.size() > before);
        }
        REQUIRE_FALSE(decoded.empty());
        if (token == 0x1fe) { // The retained lift macro fills the entire interval.
            REQUIRE(decoded.size() == frames * channels);
        }
        {
            auto referencePcm = decoded;
            // Independent oracle: every native macro now uses this same PCM
            // boundary, regardless of which gameplay caller starts it.
            for (f32& sample : referencePcm) {
                sample *= calibration->gain;
            }
            for (const f32 volume : {0.0f, 128.0f / 255, 1.0f}) {
                CAPTURE(volume);
                AudioMixer mixer(48000);
                AudioMixer referenceMixer(48000);
                SoundPlayer output(mixer);
                output.setCategoryVolume(SoundCategory::Effects, volume);
                const auto handle = sounds.play(output, token, 1, false);
                REQUIRE(handle != kNoSound);
                auto reference = referenceMixer.createStream(source->desc());
                reference->push(referencePcm);
                reference->finish();
                reference->setVolume(volume);
                std::array<f32, 1600> actual{};
                std::array<f32, 1600> expected{};
                f64 energy = 0;
                f32 peak = 0;
                for (usize tick = 0; tick < 120; ++tick) {
                    CAPTURE(tick, decoded.size());
                    output.update();
                    mixer.mix(actual);
                    referenceMixer.mix(expected);
                    REQUIRE(actual == expected);
                    for (const f32 sample : actual) {
                        energy += static_cast<f64>(sample) * sample;
                        peak = std::max(peak, std::abs(sample));
                    }
                }
                CAPTURE(energy, peak);
                CHECK(peak <= AudioMixer::kCeiling);
                if (volume == 0) {
                    CHECK(energy == 0);
                } else {
                    CHECK(energy > .000001);
                }
                output.stop(handle);
                mixer.mix(actual); // Drain the common stop ramp for looping macros.
                output.update();
                CHECK_FALSE(output.isPlaying(handle));
            }
        }
    }
}
