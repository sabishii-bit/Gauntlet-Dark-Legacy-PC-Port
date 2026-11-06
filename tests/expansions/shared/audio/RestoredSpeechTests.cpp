#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/MusicDeclicker.h"

#include "expansions/shared/audio/RestoredSpeech.h"

namespace {
using namespace gdl;

class Samples final : public StreamSource {
public:
    Samples(std::vector<f32> data, AudioStreamDesc format = {8000, 1}, usize chunk = 1024)
        : m_data(std::move(data)), m_format(format), m_chunk(chunk) {}

    AudioStreamDesc desc() const override { return m_format; }
    bool read(std::vector<f32>& output, usize frames) override {
        ++reads;
        CHECK(frames <= 1024);
        if (m_at == m_data.size()) {
            return false;
        }
        const usize count = std::min(
            {frames * m_format.channels, m_chunk * m_format.channels, m_data.size() - m_at});
        const auto source = std::span(m_data).subspan(m_at, count);
        output.insert(output.end(), source.begin(), source.end());
        m_at += count;
        return true;
    }
    void rewind() override { m_at = 0; }
    usize reads = 0;

private:
    std::vector<f32> m_data;
    AudioStreamDesc m_format;
    usize m_chunk;
    usize m_at = 0;
};

std::shared_ptr<Samples> clip(const std::vector<f32>& data, AudioStreamDesc format = {8000, 1},
                              usize chunk = 1024) {
    return std::make_shared<Samples>(data, format, chunk);
}

std::vector<f32> render(StreamSource& source, usize chunk) {
    std::vector<f32> result;
    for (usize calls = 0; calls < 100000; ++calls) {
        const usize before = result.size();
        const bool more = source.read(result, chunk);
        if (!more) {
            CHECK(result.size() == before);
            return result;
        }
        REQUIRE(result.size() > before);
        REQUIRE(result.size() - before <= std::min<usize>(chunk, 32768) * source.desc().channels);
    }
    FAIL("restored speech did not terminate");
    return {};
}

std::vector<f32> reference(const std::vector<f32>& input, AudioStreamDesc format) {
    MusicDeclicker first;
    first.reset(format.sampleRate, format.channels, MusicDeclicker::Pass::Impulses);
    std::vector<f32> intermediate;
    first.feed(input, intermediate);
    first.finish(intermediate);
    MusicDeclicker second;
    second.reset(format.sampleRate, format.channels, MusicDeclicker::Pass::ShortBursts);
    std::vector<f32> output;
    second.feed(intermediate, output);
    second.finish(output);
    return output;
}

std::vector<f32> tone(usize frames, AudioStreamDesc format) {
    std::vector<f32> result(frames * format.channels);
    for (usize frame = 0; frame < frames; ++frame) {
        result[frame * format.channels] =
            0.2f * std::sin(2 * std::numbers::pi_v<f32> * 440 * static_cast<f32>(frame) /
                            static_cast<f32>(format.sampleRate));
    }
    return result;
}

TEST_CASE("restored speech matches both native-rate passes independently of read chunking",
          "[expansions][audio][restored-speech]") {
    constexpr AudioStreamDesc kFormat{8000, 2};
    auto damaged = tone(5000, kFormat);
    damaged[usize{2000} * 2] = 0.95f;
    const auto expected = reference(damaged, kFormat);
    const usize inputChunk = GENERATE(usize{1}, usize{73}, usize{1024});
    const usize outputChunk = GENERATE(usize{1}, usize{317}, usize{32768});
    RestoredSpeech source({clip(damaged, kFormat, inputChunk)});
    CHECK(source.desc().sampleRate == kFormat.sampleRate);
    CHECK(source.desc().channels == kFormat.channels);
    CHECK(render(source, outputChunk) == expected);
    source.rewind();
    CHECK(render(source, 419) == expected);
}

TEST_CASE("restored speech does not mistake filter lookahead or tiny tails for end of audio",
          "[expansions][audio][restored-speech]") {
    const usize frames = GENERATE(usize{0}, usize{1}, usize{2}, usize{17}, usize{100}, usize{441});
    const u32 channels = GENERATE(1U, 2U);
    const AudioStreamDesc format{8000, channels};
    const std::vector<f32> input(frames * channels, 0.2f);
    RestoredSpeech source({clip(input, format, 1)});
    const auto actual = render(source, 1);
    CHECK(actual.size() == input.size());
    CHECK(actual == reference(input, format));
    source.rewind();
    CHECK(render(source, 32768) == actual);
}

TEST_CASE("restored speech finishes and resets restoration separately for every clip",
          "[expansions][audio][restored-speech]") {
    constexpr AudioStreamDesc kFormat{8000, 1};
    auto first = tone(1300, kFormat);
    first[400] = 0.98f;
    auto second = tone(1177, kFormat);
    second[400] = -0.98f;
    const std::vector<f32> last{0.7f};
    auto expected = reference(first, kFormat);
    const auto secondRestored = reference(second, kFormat);
    expected.insert(expected.end(), secondRestored.begin(), secondRestored.end());
    const auto lastRestored = reference(last, kFormat);
    expected.insert(expected.end(), lastRestored.begin(), lastRestored.end());
    RestoredSpeech source({clip({}), clip(first), clip({}), clip(second), clip(last), clip({})});
    CHECK(render(source, 79) == expected);
    source.rewind();
    std::vector<f32> prefix;
    REQUIRE(source.read(prefix, 41));
    source.rewind();
    CHECK(render(source, 32768) == expected);
    // Reusing a clip object must restart it, not expose the previous cursor.
    auto repeated = clip(first);
    RestoredSpeech twice({repeated, repeated});
    auto doubled = reference(first, kFormat);
    const auto one = doubled;
    doubled.insert(doubled.end(), one.begin(), one.end());
    CHECK(render(twice, 32768) == doubled);
}

TEST_CASE("restored speech bypasses unsupported low restoration rates and bounds output",
          "[expansions][audio][restored-speech]") {
    const u32 rate = GENERATE(1000U, 7999U);
    const AudioStreamDesc format{rate, 2};
    auto input = tone(70000, format);
    input[100] = -0.99f;
    RestoredSpeech source({clip(input, format)});
    std::vector<f32> output{0.125f};
    CHECK_FALSE(source.read(output, 0));
    REQUIRE(source.read(output, std::numeric_limits<usize>::max()));
    CHECK(output.size() == 1 + 32768 * 2);
    CHECK(output.front() == 0.125f);
    CHECK(std::equal(output.begin() + 1, output.end(), input.begin()));
    source.rewind();
    CHECK(render(source, 997) == input);
}

TEST_CASE(
    "restored speech leaves silence exact and repairs clicks without touching the clean channel",
    "[expansions][audio][restored-speech]") {
    constexpr AudioStreamDesc kFormat{24000, 2};
    const std::vector<f32> silence(3000, 0);
    RestoredSpeech silent({clip(silence, kFormat)});
    CHECK(render(silent, 317) == silence);
    const auto clean = tone(12000, kFormat);
    auto damaged = clean;
    for (const usize frame : {usize{2000}, usize{6000}, usize{8999}}) {
        damaged[frame * 2] = 0.95f;
    }
    RestoredSpeech source({clip(damaged, kFormat)});
    const auto restored = render(source, 317);
    REQUIRE(restored.size() == damaged.size());
    f64 before = 0;
    f64 after = 0;
    for (usize i = 0; i < clean.size(); ++i) {
        before += std::pow(damaged[i] - clean[i], 2);
        after += std::pow(restored[i] - clean[i], 2);
        if (i % 2 != 0) {
            REQUIRE(restored[i] == clean[i]);
        }
    }
    CHECK(after < before * 0.01);
}

TEST_CASE("restored speech safely rejects null mixed and invalid source formats",
          "[expansions][audio][restored-speech]") {
    std::vector<std::shared_ptr<StreamSource>> sources;
    SECTION("empty") {}
    SECTION("null") {
        sources.push_back({});
    }
    SECTION("bad rate or channels") {
        const auto format =
            GENERATE(AudioStreamDesc{0, 1}, AudioStreamDesc{999, 1}, AudioStreamDesc{192001, 1},
                     AudioStreamDesc{8000, 0}, AudioStreamDesc{8000, 3});
        sources.push_back(clip({}, format));
    }
    SECTION("mixed rates") {
        sources = {clip({}, {8000, 1}), clip({}, {16000, 1})};
    }
    SECTION("mixed channels") {
        sources = {clip({}, {8000, 1}), clip({}, {8000, 2})};
    }
    SECTION("unbounded empty sequence") {
        sources.assign(65, clip({}));
    }
    RestoredSpeech source(std::move(sources));
    CHECK(source.desc().sampleRate == 0);
    CHECK(source.desc().channels == 0);
    std::vector<f32> output{0.25f};
    CHECK_FALSE(source.read(output, 1024));
    CHECK(output == std::vector<f32>{0.25f});
    source.rewind();
    CHECK_FALSE(source.read(output, 1024));
}

TEST_CASE("restored speech bounds nonprogressing inputs and rejects malformed frames",
          "[expansions][audio][restored-speech]") {
    SECTION("true without samples cannot busy-loop") {
        auto stalled = clip({0.1f}, {8000, 1}, 0);
        RestoredSpeech source({stalled});
        std::vector<f32> output;
        CHECK_FALSE(source.read(output, 1024));
        CHECK(stalled->reads == 1);
        CHECK(output.empty());
    }
    SECTION("incomplete interleaved frame") {
        RestoredSpeech source({clip({0.1f}, {8000, 2})});
        std::vector<f32> output;
        CHECK_FALSE(source.read(output, 1024));
        CHECK(output.empty());
    }
    SECTION("nonfinite samples") {
        RestoredSpeech source({clip({std::numeric_limits<f32>::quiet_NaN()})});
        std::vector<f32> output;
        CHECK_FALSE(source.read(output, 1024));
        CHECK(output.empty());
    }
    SECTION("source cannot exceed its requested block") {
        class Overproducer final : public StreamSource {
        public:
            AudioStreamDesc desc() const override { return {8000, 1}; }
            bool read(std::vector<f32>& output, usize frames) override {
                output.resize(frames + 1, 0);
                return true;
            }
            void rewind() override {}
        };
        RestoredSpeech source({std::make_shared<Overproducer>()});
        std::vector<f32> output{0.25f};
        CHECK_FALSE(source.read(output, 1024));
        CHECK(output == std::vector<f32>{0.25f});
    }
}

} // namespace
