#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/n64/audio/LegendsAmbient.h"
#include "expansions/n64/audio/LegendsSounds.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using namespace gdl::formats;

LegendsN64Ambient emitter(u32 identity = 0) {
    return {identity, {0, 0, 0}, 7, 100, 45, 1};
}
} // namespace

TEST_CASE("N64 ambient attenuation uses native camera attention distance and hysteresis",
          "[legends][audio][native-ambient]") {
    LegendsAmbient ambient;
    const std::array definitions{emitter()};
    REQUIRE(ambient.reset(definitions));
    CHECK(ambient.size() == 1);
    CHECK(ambient.update({0, 0, 101}).empty());
    auto voices = ambient.update({0, 0, 0});
    REQUIRE(voices.size() == 1);
    CHECK(voices.front().volume == 1);
    CHECK(voices.front().nativeIndex == 0);
    CHECK(voices.front().token == 7);
    CHECK(voices.front().position == Vec3{0});
    CHECK(voices.front().radius == 100);
    // A .1 change does not cross the native squared .02 threshold.
    CHECK(ambient.update({0, 10, 0}).front().volume == 1);
    // A .15 change does; game byte gain is then quantized to MusyX seven bits.
    const f32 expected = 107.0f / 127;
    CHECK(ambient.update({0, 15, 0}).front().volume == Catch::Approx(expected));
    CHECK(ambient.update({0, 20, 0}).front().volume == Catch::Approx(expected));
    CHECK(ambient.update({0, 0, 0}).front().volume == 1);
    // Vertical distance counts too. This is not a ground-plane listener test.
    CHECK(ambient.update({0, 101, 0}).empty());
    voices = ambient.update({0, 100, 0});
    REQUIRE(voices.size() == 1);
    CHECK(voices.front().volume == 0);
    CHECK(ambient.update({0, 0, 0}, true).empty());
    CHECK(ambient.update({0, 0, 0}).front().volume == 1);
}

TEST_CASE("N64 ambient placement identities remain independent and malformed state is refused",
          "[legends][audio][native-ambient]") {
    LegendsAmbient ambient;
    auto second = emitter(1);
    second.position = {200, 0, 0};
    const std::array definitions{emitter(), second};
    REQUIRE(ambient.reset(definitions));
    auto voices = ambient.update({100, 0, 0});
    REQUIRE(voices.size() == 2);
    CHECK(voices[0].nativeIndex != voices[1].nativeIndex);
    CHECK(voices[0].token == voices[1].token);
    CHECK(voices[0].volume == 0);
    CHECK(voices[1].volume == 0);
    voices = ambient.update({200, 0, 0});
    REQUIRE(voices.size() == 1);
    CHECK(voices.front().nativeIndex == 1);
    CHECK(voices.front().volume == 1);
    CHECK(ambient.update({std::numeric_limits<f32>::quiet_NaN(), 0, 0}).empty());
    CHECK(ambient.update({std::numeric_limits<f32>::max(), 0, 0}).empty());
    auto invalid = emitter();
    SECTION("duplicate identities") {
        const std::array duplicate{invalid, invalid};
        CHECK_FALSE(ambient.reset(duplicate));
    }
    SECTION("invalid radius") {
        invalid.radius = 0;
        CHECK_FALSE(ambient.reset(std::span(&invalid, 1)));
    }
    SECTION("nonfinite radius") {
        invalid.radius = std::numeric_limits<f32>::infinity();
        CHECK_FALSE(ambient.reset(std::span(&invalid, 1)));
    }
    SECTION("nonfinite position") {
        invalid.position.y = std::numeric_limits<f32>::quiet_NaN();
        CHECK_FALSE(ambient.reset(std::span(&invalid, 1)));
    }
    SECTION("native bounded radius") {
        invalid.radius = 32768;
        CHECK_FALSE(ambient.reset(std::span(&invalid, 1)));
    }
    SECTION("empty reset") {
        CHECK(ambient.reset({}));
    }
    CHECK(ambient.size() == 0);
    CHECK(ambient.update({0, 0, 0}).empty());
}

TEST_CASE("Disabled native ambient cues do not acquire voices",
          "[legends][audio][native-ambient]") {
    LegendsAmbient ambient;
    auto disabled = emitter();
    disabled.token = 0xffff;
    REQUIRE(ambient.reset(std::span(&disabled, 1)));
    CHECK(ambient.update(disabled.position).empty());
}

TEST_CASE("Native ambient macros preserve authored loops and finite samples",
          "[legends][audio][native-ambient][native-effects][assets]") {
    if constexpr (!GDL_LEGENDS_AUDIO) {
        SKIP("Native MusyX audio is disabled in this build");
    }
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "audio/gauntlet.proj")) {
        SKIP("Native N64 audio extraction is not installed");
    }
    LegendsSounds sounds;
    REQUIRE(sounds.openN64(root));
    for (const u16 token : std::array<u16, 4>{0, 7, 276, 277}) {
        CAPTURE(token);
        AudioMixer mixer(48000);
        SoundPlayer output(mixer);
        // Authored macro/sample loops must remain live; PCM loop=true could hide
        // an incorrectly terminated macro and produce an artificial restart.
        const auto handle = sounds.play(output, token, 1, false);
        REQUIRE(handle != kNoSound);
        std::array<f32, 1600> samples{};
        f64 energy = 0;
        f64 lateEnergy = 0;
        // Cue276 lowers the sample by two octaves, so its finite sample lasts
        // over eight seconds. Leave sufficient time for its native reverb too.
        const usize ticks = token == 276 ? 1200 : 600;
        for (usize tick = 0; tick < ticks; ++tick) {
            mixer.mix(samples);
            REQUIRE(std::ranges::all_of(samples, [](f32 sample) { return std::isfinite(sample); }));
            for (const f32 sample : samples) {
                energy += static_cast<f64>(sample) * sample;
                if (tick >= ticks - 60) {
                    lateEnergy += static_cast<f64>(sample) * sample;
                }
            }
            output.update();
        }
        CHECK(output.isPlaying(handle));
        CHECK(energy > .001);
        if (token == 276) {
            // Macro318 starts sample216 (loop length zero) at offset3987, then
            // waits for key-off. Do not turn its one-shot sample into a loop.
            CHECK(lateEnergy < energy * .000001);
        } else {
            // Macros98/425/319 use samples8/68 with full-length native loops.
            CHECK(lateEnergy > .001);
        }
        output.stop(handle);
        CHECK_FALSE(output.isPlaying(handle));
    }
}
