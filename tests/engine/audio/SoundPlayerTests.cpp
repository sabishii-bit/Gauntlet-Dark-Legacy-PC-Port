#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundClip.h"
#include "engine/audio/SoundPlayer.h"

namespace {

using namespace gdl;

SoundClip tone(u32 rate, usize frames, f32 value) {
    SoundClip clip;
    clip.sampleRate = rate;
    clip.channels = 1;
    clip.samples.assign(frames, value);
    return clip;
}

/** Pulls `frames` stereo frames through the mixer and returns them. */
std::vector<f32> pull(AudioMixer& mixer, usize frames) {
    std::vector<f32> out(frames * 2, 0.0f);
    mixer.mix(out);
    return out;
}

TEST_CASE("one-shot sounds play once and are dropped when drained", "[audio][player]") {
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const SoundClip clip = tone(48000, 100, 0.5f);
    SoundSequence sequence;
    sequence.steps.push_back(SoundSequenceStep{&clip, false, false});
    const SoundHandle handle = player.play(sequence, 1.0f);
    REQUIRE(handle != kNoSound);
    REQUIRE(player.isPlaying(handle));
    REQUIRE(player.voiceCount() == 1);

    std::vector<f32> out = pull(mixer, 100);
    REQUIRE(out[0] == 0.5f);
    REQUIRE(out[199] == 0.5f);
    player.update();
    out = pull(mixer, 10);
    REQUIRE(out[0] == 0.0f);
    player.update();
    REQUIRE_FALSE(player.isPlaying(handle));
    REQUIRE(player.voiceCount() == 0);
}

TEST_CASE("looping sequences keep feeding and stop on request", "[audio][player]") {
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const SoundClip intro = tone(48000, 48000, 0.25f);
    const SoundClip loop = tone(48000, 48000, 0.75f);
    SoundSequence sequence;
    sequence.steps.push_back(SoundSequenceStep{&intro, false, false});
    sequence.steps.push_back(SoundSequenceStep{&loop, true, true});
    sequence.volume = 1.0f;
    const SoundHandle handle = player.play(sequence, 1.0f);
    REQUIRE(sequence.loops());

    std::vector<f32> out = pull(mixer, 48000);
    REQUIRE(out[0] == 0.25f);
    for (int second = 0; second < 5; ++second) {
        player.update();
        out = pull(mixer, 48000);
        REQUIRE(out[0] == 0.75f);
        REQUIRE(out[usize{2} * 47999] == 0.75f);
    }
    REQUIRE(player.isPlaying(handle));
    player.stop(handle);
    player.update();
    REQUIRE_FALSE(player.isPlaying(handle));
    REQUIRE(pull(mixer, 10)[0] == 0.0f);
}

TEST_CASE("clips at another rate are resampled into the voice", "[audio][player]") {
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const SoundClip first = tone(24000, 240, 0.5f);
    const SoundClip second = tone(12000, 120, 1.0f);
    SoundSequence sequence;
    sequence.steps.push_back(SoundSequenceStep{&first, false, false});
    sequence.steps.push_back(SoundSequenceStep{&second, false, false});
    REQUIRE(player.play(sequence, 0.5f) != kNoSound);
    std::vector<f32> out = pull(mixer, 480 + 480);
    REQUIRE(out[0] == 0.25f);
    REQUIRE(out[usize{2} * 470] == 0.25f);
    REQUIRE(out[usize{2} * 500] == 0.5f);
    REQUIRE(out[usize{2} * 950] == 0.5f);

    const SoundSequence empty;
    REQUIRE(player.play(empty, 1.0f) == kNoSound);
    player.stopAll();
    player.update();
    REQUIRE(player.voiceCount() == 0);
}

} // namespace
