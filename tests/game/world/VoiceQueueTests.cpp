#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundClip.h"
#include "engine/core/Types.h"

#include "game/world/VoiceQueue.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("voice queues serialize clips and reject excess waiting without silencing the first",
          "[game][voice-queue]") {
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SoundClip clip;
    clip.sampleRate = 48000;
    clip.channels = 1;
    clip.samples.assign(48000, 0.25f);
    SoundSequence sequence;
    sequence.steps.push_back({&clip, false, false});
    VoiceQueue queue;
    queue.bind(&player);
    const auto first = queue.queue(sequence, 0.5f);
    REQUIRE(first != kNoSound);
    CHECK(queue.room(1));
    const auto second = queue.queue(sequence, 0.25f);
    REQUIRE(second != kNoSound);
    CHECK(player.voiceCount() == 1);
    CHECK(queue.backlog() == Approx(2));
    CHECK_FALSE(queue.room(1));
    queue.hold(true);
    CHECK_FALSE(queue.room(-1));
    CHECK(queue.queue(sequence, 1) == kNoSound);
    queue.hold(false);
    queue.update(-1);
    CHECK(queue.backlog() == Approx(2));
    std::vector<f32> samples(96000);
    mixer.mix(samples);
    CHECK(samples.back() == Approx(0.125f));
    player.update();
    queue.update(1);
    CHECK(queue.backlog() == Approx(1));
    mixer.mix(samples);
    CHECK(samples.back() == Approx(0.0625f));
    player.update();
    queue.update(1);
    CHECK(queue.backlog() == Approx(0));
    CHECK(queue.room(0));
}

TEST_CASE("voice queues cap pending clips and recover when their playback is stopped",
          "[game][voice-queue]") {
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SoundClip clip;
    clip.sampleRate = 48000;
    clip.channels = 1;
    clip.samples.assign(48000, 0.1f);
    SoundSequence sequence;
    sequence.steps.push_back({&clip, false, false});
    VoiceQueue queue;
    CHECK(queue.queue(sequence, 1) == kNoSound);
    queue.bind(&player);
    SoundHandle tail = kNoSound;
    for (usize i = 0; i < VoiceQueue::kMost; ++i) {
        tail = queue.queue(sequence, 1);
        REQUIRE(tail != kNoSound);
    }
    CHECK_FALSE(queue.room(-1));
    CHECK(queue.queue(sequence, 1) == kNoSound);
    player.stop(tail);
    CHECK(queue.backlog() == 0);
    CHECK(queue.queue(sequence, 1) != kNoSound);
    queue.hold(true);
    queue.clear();
    CHECK_FALSE(queue.held());
    CHECK(queue.backlog() == 0);
}
} // namespace
