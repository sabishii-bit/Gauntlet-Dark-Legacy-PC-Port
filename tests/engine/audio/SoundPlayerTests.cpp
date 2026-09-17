#include <algorithm>
#include <memory>
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

TEST_CASE("a chained sound starts when the one before it ends", "[audio][player]") {
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const SoundClip first = tone(48000, 100, 0.5f);
    const SoundClip second = tone(48000, 100, 0.25f);
    SoundSequence a;
    a.steps.push_back(SoundSequenceStep{&first, false, false});
    SoundSequence b;
    b.steps.push_back(SoundSequenceStep{&second, false, false});
    const SoundHandle one = player.play(a);
    const SoundHandle two = player.playAfter(one, b);
    REQUIRE(two != kNoSound);
    REQUIRE(two != one);
    REQUIRE(player.isPlaying(two));
    REQUIRE(player.voiceCount() == 1);

    std::vector<f32> out = pull(mixer, 100);
    REQUIRE(out[0] == 0.5f);
    player.update();
    out = pull(mixer, 10);
    player.update(); // drops the first voice and starts the second
    REQUIRE_FALSE(player.isPlaying(one));
    REQUIRE(player.isPlaying(two));
    REQUIRE(player.voiceCount() == 1);
    out = pull(mixer, 100);
    REQUIRE(out[0] == 0.25f);

    // Chaining after nothing starts at once; a stopped chain never starts.
    const SoundHandle three = player.playAfter(kNoSound, a);
    REQUIRE(player.voiceCount() == 2);
    const SoundHandle four = player.playAfter(three, b);
    REQUIRE(player.isPlaying(four));
    player.stop(four);
    REQUIRE_FALSE(player.isPlaying(four));
    const SoundHandle five = player.playAfter(three, b);
    player.stopAll();
    REQUIRE_FALSE(player.isPlaying(five));
    REQUIRE(player.playAfter(three, SoundSequence{}) == kNoSound);
}

TEST_CASE("category and master volumes scale voices, live and on start", "[audio][player]") {
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    const SoundClip clip = tone(48000, 400, 1.0f);
    SoundSequence sequence;
    sequence.steps.push_back(SoundSequenceStep{&clip, false, false});
    player.setCategoryVolume(SoundCategory::Music, 0.5f);
    REQUIRE(player.categoryVolume(SoundCategory::Music) == 0.5f);
    REQUIRE(player.categoryVolume(SoundCategory::Effects) == 1.0f);

    player.play(sequence, 0.5f, SoundCategory::Music);
    std::vector<f32> out = pull(mixer, 10);
    REQUIRE(out[0] == 0.25f);

    player.setMasterVolume(0.5f);
    out = pull(mixer, 10);
    REQUIRE(out[0] == 0.125f);

    player.setCategoryVolume(SoundCategory::Music, 1.0f);
    out = pull(mixer, 10);
    REQUIRE(out[0] == 0.25f);
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

/** A source of `total` frames of one value, counting how often it starts over. */
class ToneSource final : public StreamSource {
public:
    ToneSource(usize total, f32 value) : m_total(total), m_value(value) {}
    AudioStreamDesc desc() const override { return AudioStreamDesc{48000, 1}; }
    bool read(std::vector<f32>& out, usize frames) override {
        if (m_read >= m_total) {
            return false;
        }
        const usize count = std::min(frames, m_total - m_read);
        out.insert(out.end(), count, m_value);
        m_read += count;
        return true;
    }
    void rewind() override {
        m_read = 0;
        ++rewinds;
    }
    int rewinds = 0;

private:
    usize m_total;
    f32 m_value;
    usize m_read = 0;
};

TEST_CASE("stream sources play ahead of the mixer, looping or ending", "[audio][player]") {
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    REQUIRE(player.playStream(nullptr, false) == kNoSound);

    // One second of tone, once: it drains after a second and the voice goes.
    auto once = std::make_shared<ToneSource>(48000, 0.5f);
    const SoundHandle single = player.playStream(once, false, 1.0f, SoundCategory::Music);
    REQUIRE(single != kNoSound);
    REQUIRE(player.voiceCount() == 1);
    std::vector<f32> out = pull(mixer, 24000);
    REQUIRE(out[0] == 0.5f);
    player.update();
    out = pull(mixer, 24000);
    REQUIRE(out[usize{2} * 23999] == 0.5f);
    player.update();
    out = pull(mixer, 100);
    REQUIRE(out[0] == 0.0f);
    player.update();
    REQUIRE_FALSE(player.isPlaying(single));
    REQUIRE(once->rewinds == 0);

    // Looping, the same source starts over every second and follows the music volume.
    auto loop = std::make_shared<ToneSource>(48000, 0.5f);
    const SoundHandle music = player.playStream(loop, true, 1.0f, SoundCategory::Music);
    for (int second = 0; second < 4; ++second) {
        player.update();
        out = pull(mixer, 48000);
        REQUIRE(out[0] == 0.5f);
        REQUIRE(out[usize{2} * 47999] == 0.5f);
    }
    REQUIRE(loop->rewinds >= 3);
    player.setCategoryVolume(SoundCategory::Music, 0.5f);
    player.update();
    out = pull(mixer, 100);
    REQUIRE(out[0] == 0.25f);
    REQUIRE(player.isPlaying(music));
    player.stop(music);
    player.update();
    REQUIRE_FALSE(player.isPlaying(music));
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
