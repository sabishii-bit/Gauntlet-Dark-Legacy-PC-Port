#include <array>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/io/NativeStateDigest.h"

#include "TestSupport.h"
#include "game/audio/LevelSoundscape.h"

namespace {
using namespace gdl;
using namespace gdl::game;

template <class T> auto nativeHash(const T& owner) {
    NativeStateDigest digest;
    REQUIRE(owner.appendNativeDigest(digest));
    REQUIRE(digest.checkedFinish());
    return digest.finish();
}
template <class T> auto savedHash(const T& owner, const typename T::LocalSnapshot& saved) {
    NativeStateDigest digest;
    REQUIRE(owner.appendLocalDigest(digest, saved));
    REQUIRE(digest.checkedFinish());
    return digest.finish();
}

TEST_CASE("native voice hashes include silent admission holds and logical predecessors",
          "[native-digest][audio-native-digest]") {
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    SoundClip clip;
    clip.sampleRate = 48000;
    clip.channels = 1;
    clip.samples.assign(48000, 0.125f);
    SoundSequence sequence;
    sequence.bank = "TEST";
    sequence.steps.push_back({&clip, false, false});
    VoiceQueue audible;
    VoiceQueue silent;
    audible.bind(&output);
    silent.bind(nullptr);
    SceneSoundEvents left;
    SceneSoundEvents right;
    left.open(42, &output);
    right.open(42, nullptr);
    audible.recordTo(&left);
    silent.recordTo(&right);
    const auto append = [&](VoiceQueue& queue, SceneSoundEvents& events, u64 tick, u32 cue) {
        events.beginTick(tick);
        {
            const auto scope = events.scoped({7, 3, 1, 0}, cue);
            queue.queue(sequence, 1);
        }
        return events.endTick();
    };
    append(audible, left, 1, 2);
    append(silent, right, 1, 2);
    CHECK(nativeHash(audible) == nativeHash(silent));
    VoiceQueue::LocalSnapshot saved;
    silent.saveLocal(saved);
    const auto initial = nativeHash(silent);
    silent.hold(true);
    CHECK(nativeHash(silent) != initial);
    CHECK(savedHash(silent, saved) == initial);
    REQUIRE(silent.restoreLocal(saved));
    append(audible, left, 2, 3);
    append(silent, right, 2, 9);
    REQUIRE(audible.backlog() == silent.backlog());
    CHECK(nativeHash(audible) != nativeHash(silent));
    REQUIRE(silent.restoreLocal(saved));
    append(silent, right, 2, 3);
    CHECK(nativeHash(audible) == nativeHash(silent));
    for (usize distance = 1; distance <= 8; ++distance) {
        CAPTURE(distance);
        REQUIRE(silent.restoreLocal(saved));
        for (usize i = 0; i < distance; ++i) {
            silent.update(1.0f / 60);
        }
        CHECK(nativeHash(silent) != initial);
        CHECK(savedHash(silent, saved) == initial);
    }
    NativeStateDigest foreign;
    CHECK_FALSE(audible.appendLocalDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());
    right.close();
    NativeStateDigest stale;
    CHECK_FALSE(silent.appendLocalDigest(stale, saved));
}

TEST_CASE("native soundscape hashes requests and hidden duck state but not mixer progress",
          "[native-digest][audio-native-digest][local-rewind]") {
    const auto root = test::scratchDirectory("audio-native-digest");
    AudioMixer mixer(48000);
    SoundPlayer output(mixer);
    const LevelAudioInfo info{.bank = {}, .stream = {}, .areas = 3};
    LevelSoundscape audible;
    LevelSoundscape silent;
    audible.open(root, &output, &info);
    silent.open(root, nullptr, &info);
    CHECK(nativeHash(audible) == nativeHash(silent));
    LevelSoundscape::LocalSnapshot before;
    silent.saveLocal(before);
    const auto initial = nativeHash(silent);
    const auto changes = [&](auto change) {
        REQUIRE(silent.restoreLocal(before));
        change(silent);
        CHECK(nativeHash(silent) != initial);
        CHECK(savedHash(silent, before) == initial);
    };
    changes([](auto& scene) { scene.duckMusic(0.125f, 0.5f); });
    changes([](auto& scene) { scene.setPlayerCount(4); });
    changes([](auto& scene) { scene.selectMusicArea(2, MusicSwitch::Faded); });
    changes([](auto& scene) { scene.holdNarration(true); });
    changes([](auto& scene) { scene.bossAwake(true); });
    REQUIRE(silent.restoreLocal(before));
    audible.duckMusic(1, 0.75f);
    silent.duckMusic(1, 0.75f);
    audible.servicePresentation(0.25f);
    std::array<f32, 128> samples{};
    mixer.mix(samples);
    output.update();
    CHECK(nativeHash(audible) == nativeHash(silent));
    NativeStateDigest foreign;
    CHECK_FALSE(audible.appendLocalDigest(foreign, before));
    silent.close();
    NativeStateDigest stale;
    CHECK_FALSE(silent.appendLocalDigest(stale, before));
    audible.close();
}

TEST_CASE("native soundscape digest bytes stay pinned for distinct requests and holds",
          "[native-digest][audio-native-digest][native-digest-pin]") {
    const auto root = test::scratchDirectory("audio-native-digest-pin");
    const LevelAudioInfo info{.bank = {}, .stream = {}, .areas = 3};
    LevelSoundscape soundscape;
    soundscape.open(root, nullptr, &info);
    soundscape.duckMusic(0.125f, 0.5f);
    soundscape.setPlayerCount(3);
    soundscape.selectMusicArea(2, MusicSwitch::Faded);
    soundscape.holdNarration(true);
    soundscape.bossAwake(true);
    // SHA-256 of the native digest of the soundscape above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "fee4abbadaa02641290b70248d85675e94d2b45c8126ec0510a6511a0818d98b";
    CHECK(test::hexBytes(nativeHash(soundscape)) == kDigest);
    soundscape.close();
}
} // namespace
