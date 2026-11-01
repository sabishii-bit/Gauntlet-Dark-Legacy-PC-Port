#include <array>
#include <map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"

#include "game/audio/SceneSoundEvents.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr FrameEventEmitter kPlayer{1, 0, 1, 0};
constexpr FrameEventEmitter kEnemy{2, 37, 5, FrameEventEmitter::kNoSeat};

struct Sounds {
    SoundClip clip{48000, 1, std::vector<f32>(48000, 0.125f)};
    SoundSequence a{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "COMMON", 3};
    SoundSequence b{{{&clip, false, false}}, 1, SoundGainCurve::Linear, "VOICE1", 3};
};

TEST_CASE("scene sounds record values before output and replay identical cues only once",
          "[scene-sound-events][local-rewind]") {
    const Sounds sounds;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SceneSoundEvents events;
    events.open(1, &player);
    usize commands = 0;
    const SoundPlayer::Capture observe(&player, [&](const auto&) { ++commands; });
    events.beginTick(1);
    SoundCueToken first;
    SoundCueToken second;
    {
        auto scope = events.scoped(kPlayer, 7);
        first = events.emit(sounds.a, 0.5f, -0.25f);
        second = events.emit(sounds.b, 0.75f, 0, first);
    }
    CHECK(commands == 0);
    CHECK(player.voiceCount() == 0);
    const std::array journal{events.endTick()};
    REQUIRE(journal.front().events.size() == 2);
    CHECK(first.event != second.event);
    CHECK(journal.front().events[0].payload.resource.bank == "COMMON");
    CHECK(journal.front().events[1].payload.resource.bank == "VOICE1");
    CHECK(journal.front().events[1].payload.after == first);
    const auto result = events.reconcile(1, journal);
    REQUIRE(result);
    CHECK(commands == 0);
    events.dispatch(result);
    CHECK(commands > 0);
    CHECK(player.voiceCount() == 1); // The second line is queued behind the first.
    const auto before = commands;
    const auto repeated = events.reconcile(1, journal);
    REQUIRE(repeated);
    CHECK(repeated.actions.empty());
    events.dispatch(repeated);
    CHECK(commands == before);
    REQUIRE(events.confirm(1));
    CHECK(events.unsupportedCount() == 0);
    events.close();
}

TEST_CASE("scene sound identity is emitter local rather than global call order",
          "[scene-sound-events][local-rewind]") {
    const Sounds sounds;
    SceneSoundEvents first;
    SceneSoundEvents reordered;
    first.open(7, nullptr);
    reordered.open(7, nullptr);
    const auto emit = [&](SceneSoundEvents& events, FrameEventEmitter emitter, u32 cue,
                          const SoundSequence& sound) {
        auto scope = events.scoped(emitter, cue);
        return events.emit(sound);
    };
    first.beginTick(1);
    const auto a = emit(first, kPlayer, 1, sounds.a);
    const auto b = emit(first, kEnemy, 2, sounds.b);
    const auto c = emit(first, kPlayer, 1, sounds.a);
    const auto one = first.endTick();
    reordered.beginTick(1);
    const auto b2 = emit(reordered, kEnemy, 2, sounds.b);
    const auto a2 = emit(reordered, kPlayer, 1, sounds.a);
    const auto c2 = emit(reordered, kPlayer, 1, sounds.a);
    const auto two = reordered.endTick();
    CHECK(a == a2);
    CHECK(b == b2);
    CHECK(c == c2);
    CHECK(a != c);
    const auto byIdentity = [](const auto& journal) {
        std::map<FrameEventIdentity, SceneSoundPayload> result;
        for (const auto& event : journal.events) {
            result.emplace(event.id, event.payload);
        }
        return result;
    };
    CHECK(byIdentity(one) == byIdentity(two));
}

TEST_CASE("corrected scene speech uses its logical predecessor not the discarded future voice",
          "[scene-sound-events][local-rewind]") {
    const Sounds sounds;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SceneSoundEvents events;
    events.open(3, &player);
    std::vector<SoundPlayer::Event> output;
    const SoundPlayer::Capture observe(&player, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            output.push_back(event);
        }
    });
    const auto capture = [&](u64 tick, u32 cue, const SoundSequence& sound, SoundCueToken after) {
        events.beginTick(tick);
        SoundCueToken token;
        {
            auto scope = events.scoped(kPlayer, cue);
            token = events.emit(sound, 1, 0, after);
        }
        return std::pair{events.endTick(), token};
    };
    const auto publish = [&](u64 tick, const auto& journal) {
        const std::array journals{journal};
        auto result = events.reconcile(tick, journals);
        REQUIRE(result);
        events.dispatch(result);
        return result;
    };
    const auto [initial, first] = capture(1, 1, sounds.a, {});
    publish(1, initial);
    const auto [wrong, discarded] = capture(2, 90, sounds.b, first);
    publish(2, wrong);
    REQUIRE(output.size() == 2);
    const auto previousHandle = output[0].handle;
    const auto wrongHandle = output[1].handle;
    const auto [corrected, next] = capture(2, 2, sounds.a, first);
    const auto correction = publish(2, corrected);
    REQUIRE(correction.unretractable == std::vector{discarded.event});
    REQUIRE(output.size() == 3);
    CHECK(output.back().after == previousHandle);
    CHECK(output.back().after != wrongHandle);
    CHECK(corrected.events.front().payload.after == first);
    CHECK(next.event != discarded.event);
    const auto [replayed, replayToken] = capture(2, 2, sounds.a, first);
    CHECK(replayed == corrected);
    CHECK(replayToken == next);
    CHECK(publish(2, replayed).actions.empty());
    CHECK(output.size() == 3);
    events.close();
}

TEST_CASE("scene sound capture rejects uncovered calls before touching the mixer",
          "[scene-sound-events][local-rewind]") {
    const Sounds sounds;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SceneSoundEvents events;
    events.open(4, &player);
    usize commands = 0;
    const SoundPlayer::Capture observe(&player, [&](const auto&) { ++commands; });
    CHECK_THROWS_AS(events.emit(sounds.a), SceneSoundEvents::Unsupported);
    events.beginTick(1);
    CHECK_THROWS_AS(events.emit(sounds.a), SceneSoundEvents::Unsupported);
    {
        auto scope = events.scoped(kPlayer, 1);
        auto loop = sounds.a;
        loop.steps[0].loopStart = loop.steps[0].loopBack = true;
        CHECK_THROWS_AS(events.emit(loop), SceneSoundEvents::Unsupported);
        auto anonymous = sounds.a;
        anonymous.bank.clear();
        CHECK_THROWS_AS(events.emit(anonymous), SceneSoundEvents::Unsupported);
        CHECK_THROWS_AS(events.dispatch({}), SceneSoundEvents::Unsupported);
    }
    const auto journal = events.endTick();
    CHECK(journal.events.empty());
    CHECK(events.unsupportedCount() == 5);
    CHECK_FALSE(events.lastUnsupported().empty());
    CHECK(commands == 0);
    CHECK(player.voiceCount() == 0);
    events.close();
}

TEST_CASE("scene sound validation rejects invalid ticks and predecessor identities atomically",
          "[scene-sound-events][local-rewind]") {
    const Sounds sounds;
    SceneSoundEvents events;
    CHECK_THROWS_AS(events.open(1, nullptr, 0), SceneSoundEvents::Unsupported);
    events.open(1, nullptr);
    CHECK_THROWS_AS(events.beginTick(0), SceneSoundEvents::Unsupported);
    CHECK_FALSE(events.recording());
    events.beginTick(1);
    SoundCueToken first;
    SoundCueToken second;
    {
        auto scope = events.scoped(kPlayer, 1);
        SoundCueToken malformed;
        malformed.event.epoch = 1;
        CHECK_FALSE(malformed);
        CHECK_THROWS_AS(events.emit(sounds.a, 1, 0, malformed), SceneSoundEvents::Unsupported);
        const SoundCueToken future{{1, 2, kPlayer, 1, 0}};
        CHECK_THROWS_AS(events.emit(sounds.a, 1, 0, future), SceneSoundEvents::Unsupported);
        const SoundCueToken sameTick{{1, 1, kPlayer, 1, 0}};
        CHECK_THROWS_AS(events.emit(sounds.a, 1, 0, sameTick), SceneSoundEvents::Unsupported);
        first = events.emit(sounds.a);
        CHECK(first.event.occurrence == 0);
        second = events.emit(sounds.b, 1, 0, first);
    }
    const std::array journal{events.endTick()};
    REQUIRE(journal[0].events.size() == 2);
    CHECK(second.event.occurrence == 1);
    auto invalid = journal;
    std::swap(invalid[0].events[0], invalid[0].events[1]);
    CHECK_THROWS_AS(events.reconcile(1, invalid), SceneSoundEvents::Unsupported);
    const auto accepted = events.reconcile(1, journal);
    REQUIRE(accepted);
    CHECK(accepted.actions.size() == 2);
    CHECK_FALSE(events.recording());
}

TEST_CASE("scene sound correction refuses a predecessor removed earlier in the replay suffix",
          "[scene-sound-events][local-rewind]") {
    const Sounds sounds;
    SceneSoundEvents events;
    events.open(1, nullptr);
    std::array<SceneSoundEvents::Journal, 2> journals;
    SoundCueToken predecessor;
    events.beginTick(1);
    {
        auto scope = events.scoped(kPlayer, 1);
        predecessor = events.emit(sounds.a);
    }
    journals[0] = events.endTick();
    events.beginTick(2);
    {
        auto scope = events.scoped(kPlayer, 2);
        events.emit(sounds.b, 1, 0, predecessor);
    }
    journals[1] = events.endTick();
    auto invalid = journals;
    invalid[0].events.clear();
    CHECK_THROWS_AS(events.reconcile(1, invalid), SceneSoundEvents::Unsupported);
    REQUIRE(events.reconcile(1, journals));
}

TEST_CASE("scene sound resources allow identical decodes but reject changed native calls",
          "[scene-sound-events][local-rewind]") {
    const Sounds sounds;
    SceneSoundEvents events;
    events.open(1, nullptr);
    auto copiedClip = sounds.clip;
    auto copied = sounds.a;
    copied.steps[0].clip = &copiedClip;
    auto changedClip = sounds.clip;
    changedClip.samples[0] = 0.75f;
    auto changed = sounds.a;
    changed.steps[0].clip = &changedClip;
    events.beginTick(1);
    {
        auto scope = events.scoped(kPlayer, 1);
        events.emit(sounds.a);
        events.emit(copied);
        CHECK_THROWS_AS(events.emit(changed), SceneSoundEvents::Unsupported);
        auto gain = sounds.a;
        gain.volume = 0.5f;
        CHECK_THROWS_AS(events.emit(gain), SceneSoundEvents::Unsupported);
        const auto tail = events.emit(sounds.a);
        CHECK(tail.event.occurrence == 2);
    }
    const std::array journal{events.endTick()};
    CHECK(journal[0].events.size() == 3);
    REQUIRE(events.reconcile(1, journal));
}

TEST_CASE("scene sound capture aborts on unwinding and old scopes cannot change a new tick",
          "[scene-sound-events][local-rewind]") {
    const Sounds sounds;
    SceneSoundEvents events;
    events.open(1, nullptr);
    events.beginTick(1);
    try {
        // NOLINTNEXTLINE(clang-analyzer-deadcode.DeadStores): the scope's unwind is the subject
        auto scope = events.scoped(kPlayer, 1);
        events.emit(sounds.a);
        throw std::runtime_error("discard this native branch");
    } catch (const std::runtime_error&) { // NOLINT(bugprone-empty-catch): the unwind is the point
    }
    CHECK_FALSE(events.recording());
    events.beginTick(1);
    {
        auto old = events.scoped(kPlayer, 1);
        events.emit(sounds.a);
        events.abortTick();
        events.beginTick(1);
    }
    {
        auto scope = events.scoped(kEnemy, 2);
        CHECK(events.emit(sounds.b).event.occurrence == 0);
    }
    const std::array journal{events.endTick()};
    REQUIRE(journal[0].events.size() == 1);
    CHECK(journal[0].events[0].payload.resource.bank == "VOICE1");
    REQUIRE(events.reconcile(1, journal));
    events.beginTick(2);
    CHECK_THROWS_AS(events.scoped({}, 1), SceneSoundEvents::Unsupported);
    CHECK_FALSE(events.recording());
    events.close();
}

TEST_CASE("scene sound dispatch stays once-only after playback ends and rejects retired deltas",
          "[scene-sound-events][local-rewind]") {
    const Sounds sounds;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SceneSoundEvents events;
    events.open(1, &player);
    usize played = 0;
    const SoundPlayer::Capture observe(&player, [&](const auto& event) {
        played += event.kind == SoundPlayer::Event::Kind::Play ? 1 : 0;
    });
    events.beginTick(1);
    {
        auto scope = events.scoped(kPlayer, 1);
        events.emit(sounds.a);
    }
    const std::array journal{events.endTick()};
    const auto first = events.reconcile(1, journal);
    REQUIRE(first);
    events.dispatch(first);
    CHECK(played == 1);
    player.stopAll();
    events.dispatch(first);
    CHECK(played == 1);
    REQUIRE(events.confirm(1));
    for (u64 tick = 2; tick <= SceneSoundEvents::Ledger::kHistory + 1; ++tick) {
        events.beginTick(tick);
        const std::array empty{events.endTick()};
        REQUIRE(events.reconcile(tick, empty));
        REQUIRE(events.confirm(tick));
    }
    CHECK_THROWS_AS(events.dispatch(first), SceneSoundEvents::Unsupported);
    CHECK(played == 1);
    events.close();
}
TEST_CASE("desired scene sounds distinguish repeating ambience from cancellable finite cues",
          "[scene-sound-events][scene-sound-loops][local-rewind]") {
    const Sounds sounds;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SceneSoundEvents events;
    events.open(61, &player);
    std::vector<SoundPlayer::Event> played;
    const SoundPlayer::Capture observe(&player, [&](const auto& event) {
        if (event.kind == SoundPlayer::Event::Kind::Play) {
            played.push_back(event);
        }
    });
    const FrameLoopKey once{kPlayer, 1, 1};
    const FrameLoopKey repeat{kEnemy, 1, 1};
    events.beginTick(1);
    SoundCueToken birth;
    {
        auto scope = events.scoped(kPlayer, 1);
        birth = events.beginCue(sounds.a);
    }
    events.desireLoop(once, sounds.a, SceneSoundPlayback::PlayOnceUntilCancelled, 1, 0, birth);
    events.desireLoop(repeat, sounds.b, SceneSoundPlayback::RepeatWhileDesired);
    const std::array frames{events.endFrame()};
    CHECK(played.empty());
    const auto first = events.reconcileFrames(1, frames);
    REQUIRE(first);
    CHECK(played.empty());
    events.dispatch(first);
    REQUIRE(played.size() == 2);
    player.stopAll(); // Both finite recordings have completed from the output's perspective.
    events.servicePlayback();
    REQUIRE(played.size() == 3);
    CHECK(played.back().bank == "VOICE1");
    events.pausePlayback();
    events.resumePlayback();
    REQUIRE(played.size() == 4);
    CHECK(played.back().bank == "VOICE1"); // A completed owned cue stays completed.
    events.dispatch(first);
    CHECK(played.size() == 4);

    events.beginTick(2);
    const std::array stopped{events.endFrame()};
    const auto stop = events.reconcileFrames(2, stopped);
    REQUIRE(stop);
    REQUIRE(stop.actions.size() == 2);
    events.dispatch(stop);
    CHECK_FALSE(player.isPlaying(played.back().handle));
    CHECK_THROWS_AS(events.dispatch(first), SceneSoundEvents::Unsupported);
    events.close();
}

TEST_CASE("corrected desired sound suffix reconciles final lifetimes without intermediate restart",
          "[scene-sound-events][scene-sound-loops][local-rewind]") {
    Sounds sounds;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SceneSoundEvents events;
    events.open(62, &player);
    usize plays = 0;
    usize stops = 0;
    const SoundPlayer::Capture observe(&player, [&](const auto& event) {
        plays += event.kind == SoundPlayer::Event::Kind::Play ? 1 : 0;
        stops += event.kind == SoundPlayer::Event::Kind::Stop ? 1 : 0;
    });
    const FrameLoopKey key{kPlayer, 1, 1};
    SoundCueToken birth;
    const auto frame = [&](u64 tick, bool desired, bool newborn = false) {
        events.beginTick(tick);
        if (newborn) {
            auto scope = events.scoped(kPlayer, 1);
            birth = events.beginCue(sounds.a);
        }
        if (desired) {
            events.desireLoop(key, sounds.a, SceneSoundPlayback::PlayOnceUntilCancelled, 1, 0,
                              birth);
        }
        return events.endFrame();
    };
    std::array original{frame(1, true, true), frame(2, true), frame(3, true)};
    auto result = events.reconcileFrames(1, original);
    REQUIRE(result);
    events.dispatch(result);
    REQUIRE(plays == 1);
    const std::array corrected{frame(2, false), frame(3, true)};
    result = events.reconcileFrames(2, corrected);
    REQUIRE(result);
    CHECK(result.actions.empty());
    events.dispatch(result);
    CHECK(plays == 1);
    CHECK(stops == 0);

    const std::array reborn{frame(2, true, true), frame(3, true)};
    result = events.reconcileFrames(2, reborn);
    REQUIRE(result);
    events.dispatch(result);
    CHECK(plays == 2);
    CHECK(stops == 1);
    // A tentative terminal at tick 2 removes the old tick-3 desire atomically.
    const std::array terminal{frame(2, false)};
    result = events.reconcileFrames(2, terminal, 2);
    REQUIRE(result);
    events.dispatch(result);
    CHECK(stops == 2);
    events.close();
}

TEST_CASE("desired sound validation rejects malformed complete sets before output",
          "[scene-sound-events][scene-sound-loops][local-rewind]") {
    const Sounds sounds;
    SceneSoundEvents events;
    events.open(63, nullptr);
    events.beginTick(1);
    const FrameLoopKey key{kPlayer, 1, 1};
    CHECK_THROWS_AS(events.desireLoop(key, sounds.a, SceneSoundPlayback::PlayOnceUntilCancelled),
                    SceneSoundEvents::Unsupported);
    events.desireLoop(key, sounds.a, SceneSoundPlayback::RepeatWhileDesired);
    CHECK_THROWS_AS(events.desireLoop(key, sounds.a, SceneSoundPlayback::RepeatWhileDesired),
                    SceneSoundEvents::Unsupported);
    const std::array frames{events.endFrame()};
    auto invalid = frames;
    invalid[0].loops[0].payload.start.event.epoch = 63;
    CHECK_THROWS_AS(events.reconcileFrames(1, invalid), SceneSoundEvents::Unsupported);
    REQUIRE(events.reconcileFrames(1, frames));
    events.beginTick(2);
    CHECK_THROWS_AS(events.scoped(kPlayer, 0x80000001U), SceneSoundEvents::Unsupported);
    CHECK_FALSE(events.recording());
}

TEST_CASE("terminal correction cannot resurrect already retired sound dispatch identities",
          "[scene-sound-events][scene-sound-loops][local-rewind]") {
    const Sounds sounds;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SceneSoundEvents events;
    events.open(64, &player);
    SceneSoundEvents::Result first;
    usize plays = 0;
    const SoundPlayer::Capture observe(&player, [&](const auto& event) {
        plays += event.kind == SoundPlayer::Event::Kind::Play ? 1 : 0;
    });
    for (u64 tick = 1; tick <= 17; ++tick) {
        events.beginTick(tick);
        if (tick == 1) {
            auto scope = events.scoped(kPlayer, 1);
            events.emit(sounds.a);
        }
        const std::array frame{events.endFrame()};
        const auto result = events.reconcileFrames(tick, frame);
        REQUIRE(result);
        events.dispatch(result);
        if (tick == 1) {
            first = result;
        }
        if (tick <= 16) {
            REQUIRE(events.confirm(tick));
        }
    }
    REQUIRE(events.reconcileFrames(17, {}, 16));
    CHECK_THROWS_AS(events.dispatch(first), SceneSoundEvents::Unsupported);
    CHECK(plays == 1);
    events.close();
}
TEST_CASE("a discarded scene recorder records its own epoch again from the first tick",
          "[scene-sound-events][local-rewind]") {
    const Sounds sounds;
    AudioMixer mixer(48000);
    SoundPlayer player(mixer);
    SceneSoundEvents events;
    usize plays = 0;
    const SoundPlayer::Capture observe(&player, [&](const auto& event) {
        plays += event.kind == SoundPlayer::Event::Kind::Play ? 1 : 0;
    });
    const auto record = [&] {
        events.beginTick(1);
        {
            auto scope = events.scoped(kPlayer, 1);
            events.emit(sounds.a);
        }
        const std::array frame{events.endFrame()};
        const auto result = events.reconcileFrames(1, frame);
        REQUIRE(result);
        events.dispatch(result);
        REQUIRE(events.confirm(1));
    };
    events.open(9, &player);
    record();
    CHECK(plays == 1);
    // Closing leaves the epoch spent; only a discarded history records it again.
    events.close();
    CHECK_THROWS_AS(events.open(9, &player), SceneSoundEvents::Unsupported);
    events.discard();
    events.open(9, &player);
    CHECK(events.epoch() == 9);
    record();
    CHECK(plays == 2);
    events.close();
}

} // namespace
