#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/audio/MusicDeclicker.h"
#include "engine/core/Types.h"

#include "fixtures/ReferenceWav.h"

namespace {
using namespace gdl;

std::vector<f32> restore(std::span<const f32> input, usize chunk, bool bursts = false) {
    MusicDeclicker filter;
    filter.reset(24000, 2,
                 bursts ? MusicDeclicker::Pass::ShortBursts : MusicDeclicker::Pass::Impulses);
    std::vector<f32> output;
    for (usize at = 0; at < input.size();) {
        const usize count = std::min(chunk, input.size() - at);
        filter.feed(input.subspan(at, count), output);
        at += count;
    }
    filter.finish(output);
    filter.finish(output);
    return output;
}

TEST_CASE("music restoration removes isolated clicks independently of feed boundaries",
          "[audio][declick]") {
    std::vector<f32> clean(48000);
    for (usize frame = 0; frame < clean.size() / 2; ++frame) {
        const f32 value =
            0.2f * std::sin(2 * std::numbers::pi_v<f32> * 440 * static_cast<f32>(frame) / 24000);
        clean[frame * 2] = value;
        clean[frame * 2 + 1] = 0;
    }
    auto damaged = clean;
    for (const usize frame : {usize{2000}, usize{8999}, usize{17000}}) {
        damaged[frame * 2] = 0.95f;
    }
    const auto repaired = restore(damaged, damaged.size());
    REQUIRE(repaired.size() == damaged.size());
    CHECK(repaired == restore(damaged, 2));
    CHECK(repaired == restore(damaged, 1994));
    f64 before = 0;
    f64 after = 0;
    for (usize i = 0; i < clean.size(); ++i) {
        before += std::pow(damaged[i] - clean[i], 2);
        after += std::pow(repaired[i] - clean[i], 2);
        if (i % 2 != 0) {
            REQUIRE(repaired[i] == 0);
        }
    }
    CHECK(after < before * 0.01);
}

TEST_CASE("music restoration preserves silence short tails and reset state", "[audio][declick]") {
    MusicDeclicker filter;
    std::vector<f32> output;
    for (const usize length : {usize{0}, usize{2}, usize{100}, usize{2000}}) {
        filter.reset(24000, 2);
        const std::vector<f32> input(length, 0);
        output.clear();
        filter.feed(input, output);
        filter.finish(output);
        CHECK(output == input);
    }
    const std::vector<f32> constant(6000, 0.2f);
    CHECK(std::ranges::equal(restore(constant, 200), constant));
}

TEST_CASE("music restoration leaves clean tones and broad percussion substantially intact",
          "[audio][declick]") {
    const u32 rate = GENERATE(24000U, 44100U, 48000U);
    const u32 channels = GENERATE(1U, 2U);
    const auto pass = GENERATE(MusicDeclicker::Pass::Impulses, MusicDeclicker::Pass::ShortBursts);
    CAPTURE(rate, channels, pass);
    std::minstd_rand random(19);
    std::vector<f32> clean;
    for (u32 frame = 0; frame < rate; ++frame) {
        const f64 time = static_cast<f64>(frame) / rate;
        f64 value = 0.2 * std::sin(2 * std::numbers::pi * 440 * time) +
                    0.08 * std::sin(2 * std::numbers::pi * 1300 * time);
        const f64 since = time - 0.5;
        if (since >= 0 && since < 0.25) {
            const f64 noise = static_cast<f64>(random()) / std::minstd_rand::max() * 2 - 1;
            value += std::min(1.0, since / 0.002) * std::exp(-since * 28) *
                     (0.18 * noise + 0.25 * std::sin(2 * std::numbers::pi * 70 * since));
        }
        clean.push_back(static_cast<f32>(value));
        if (channels == 2) {
            clean.push_back(0.1f);
        }
    }
    MusicDeclicker filter;
    filter.reset(rate, channels, pass);
    std::vector<f32> output;
    filter.feed(clean, output);
    filter.finish(output);
    REQUIRE(output.size() == clean.size());
    usize changed = 0;
    for (usize i = 0; i < clean.size(); ++i) {
        changed += clean[i] != output[i] ? 1U : 0U;
        if (channels == 2 && i % 2 != 0) {
            REQUIRE(output[i] == clean[i]);
        }
    }
    CAPTURE(changed);
    CHECK(changed < clean.size() / 1000);
    filter.reset(rate, channels, pass);
    std::vector<f32> chunked;
    const usize chunk = usize{317} * channels;
    for (usize at = 0; at < clean.size(); at += chunk) {
        filter.feed(std::span(clean).subspan(at, std::min(chunk, clean.size() - at)), chunked);
    }
    filter.finish(chunked);
    CHECK(std::ranges::equal(output, chunked));
}

TEST_CASE("music restoration rejects invalid frame input", "[audio][declick]") {
    MusicDeclicker filter;
    std::vector<f32> output;
    CHECK_THROWS(filter.reset(0, 2));
    CHECK_THROWS(filter.reset(24000, 0));
    filter.reset(24000, 2);
    CHECK_THROWS(filter.feed(std::vector<f32>{0}, output));
    filter.finish(output);
    CHECK_THROWS(filter.feed(std::vector<f32>{0, 0}, output));
}

TEST_CASE("music burst restoration repairs brief reversals without changing the other channel",
          "[audio][declick]") {
    std::vector<f32> clean(24000);
    for (usize frame = 0; frame < clean.size() / 2; ++frame) {
        clean[frame * 2] = 0.7f + 0.08f * std::sin(static_cast<f32>(frame) * 0.03f);
        clean[frame * 2 + 1] = 0.1f;
    }
    auto damaged = clean;
    constexpr usize kClickSample = usize{3000} * 2;
    damaged[kClickSample] = -0.8f;
    const auto repaired = restore(damaged, 998, true);
    REQUIRE(repaired.size() == clean.size());
    CHECK(std::ranges::equal(repaired, restore(damaged, damaged.size(), true)));
    CHECK(std::abs(repaired[kClickSample] - clean[kClickSample]) < 0.01f);
    for (usize frame = 0; frame < clean.size() / 2; ++frame) {
        REQUIRE(repaired[frame * 2 + 1] == clean[frame * 2 + 1]);
    }
}

TEST_CASE("music restoration matches the approved independent impulse audition",
          "[audio][declick][assets][declick-reference]") {
    const char* directory = std::getenv("GDL_DECLICK_REFERENCE_DIR");
    if (directory == nullptr) {
        SKIP("Set GDL_DECLICK_REFERENCE_DIR to the offline audition directory");
    }
    for (const auto* name : {"CATH2A_1", "CATH1"}) {
        CAPTURE(name);
        const std::filesystem::path root(directory);
        const auto raw = test::loadWav(root / (std::string(name) + "-original.wav"));
        const auto reference = test::loadWav(root / (std::string(name) + "-moderate.wav"));
        std::vector<f32> input;
        input.reserve(raw.samples.size());
        for (const auto sample : raw.samples) {
            input.push_back(static_cast<f32>(sample) / 32768);
        }
        const auto result = restore(input, 16000);
        REQUIRE(result.size() == reference.samples.size());
        usize differences = 0;
        f32 maxDifference = 0;
        // Without real samples on both sides, endpoint changes are not justified.
        // The offline tool extrapolates into zero padding; runtime preserves these ends.
        constexpr usize kContextSamples = usize{26} * 2;
        for (usize i = 0; i < kContextSamples; ++i) {
            REQUIRE(result[i] == input[i]);
            REQUIRE(result[result.size() - 1 - i] == input[input.size() - 1 - i]);
        }
        // The offline tool's final padded analysis window differs from streaming EOF.
        // Compare only complete windows; the exact endpoint-preservation check is above.
        constexpr usize kWindowMarginSamples = usize{825} * 2;
        for (usize i = kWindowMarginSamples; i < result.size() - kWindowMarginSamples; ++i) {
            const f32 delta = std::abs(result[i] * 32768 - static_cast<f32>(reference.samples[i]));
            differences += delta > 1 ? 1U : 0U;
            maxDifference = std::max(maxDifference, delta);
        }
        CAPTURE(differences, maxDifference);
        CHECK(differences == 0);
        if (std::string_view(name) == "CATH2A_1") {
            const auto refined = restore(result, 1000, true);
            const auto approved = test::loadWav(root / "CATH2A_1-refined.wav");
            for (usize frame = 68287; frame <= 68290; ++frame) {
                CHECK(std::abs(refined[frame * 2] * 32768 - approved.samples[frame * 2]) <= 1);
            }
        }
    }
}
} // namespace
