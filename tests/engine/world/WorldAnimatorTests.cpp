#include <algorithm>
#include <cmath>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "SampleLevel.h"
#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    WorldAnimator animator;

    explicit Fixture(std::string_view name) {
        const auto dir = test::sampleLevel(name);
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(layout.load(dir));
        REQUIRE(scene.build(layout, models, textures, device));
    }

    /** Where the blade's first vertex lands when the scene is drawn. */
    Vec3 bladeAt() {
        device.draws.clear();
        scene.draw(device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 0.0f});
        return device.draws[2].vertices[0].position;
    }
};

/** A layout of one keyed object with the given level flags. */
WorldLayout layoutWithFlags(std::string_view name, u32 flags) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "SPIN", "position": [0, 0, 0], "flags": )" +
                                          std::to_string(flags) + R"(}],
  "animations": [{"object": 0, "frames": 4, "state": 257, "start": 0,
                  "track": {"flags": 2, "frames": [0, 3], "values": [0, 1]}}]
})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    return layout;
}

TEST_CASE("native world animation fingerprints retain keys and refresh after rebinding",
          "[world][animation][native-immutable-digest]") {
    auto layout = layoutWithFlags("native-animation-immutable", 0);
    auto independentLayout = layoutWithFlags("native-animation-independent", 0);
    WorldAnimator first;
    WorldAnimator independent;
    first.bind(layout);
    independent.bind(independentLayout);
    const auto hash = [](const WorldAnimator& animator) {
        NativeStateDigest digest;
        REQUIRE(animator.appendNativeDigest(digest));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    };
    const auto initial = hash(first);
    CHECK(hash(first) == initial);
    CHECK(hash(independent) == initial);
    WorldAnimator::LocalSnapshot old;
    first.saveLocal(old);
    WorldScene scene;
    first.step(kStep, scene);
    CHECK(hash(first) != initial);
    NativeStateDigest saved;
    REQUIRE(first.appendLocalDigest(saved, old));
    CHECK(saved.finish() == initial);
    REQUIRE(first.restoreLocal(old));
    CHECK(hash(first) == initial);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& animation = const_cast<WorldAnimation&>(layout.animations().front());
    const auto authored = animation.track;
    for (u32 changed = 0; changed < 4; ++changed) {
        CAPTURE(changed);
        animation.track = authored;
        if (changed == 0) {
            ++animation.track.node;
        }
        if (changed == 1) {
            animation.track.flags ^= TrackInfo::kPitchYawRoll;
        }
        if (changed == 2) {
            --animation.track.frames.back();
        }
        if (changed == 3) {
            animation.track.values.back() += 0.125f;
        }
        first.bind(layout);
        CHECK(hash(first) != initial);
        CHECK(first.frame(0) == 0); // Changed only authored data, not the cursor.
        NativeStateDigest stale;
        CHECK_FALSE(first.appendLocalDigest(stale, old));
    }
    animation.track = authored;
    first.bind(layout);
    CHECK(hash(first) == initial);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<WorldObject&>(layout.objects().front()).position.x = 0.25f;
    first.bind(layout);
    CHECK(hash(first) != initial);
    first.clear();
    independent.clear();
    CHECK(hash(first) == hash(independent));
}

TEST_CASE("world animations turn their objects thirty frames a second and loop",
          "[world][animation]") {
    Fixture f("world-animator");
    f.animator.bind(f.layout);
    REQUIRE(f.animator.size() == 1);
    REQUIRE(f.animator.object(0) == 8);
    REQUIRE(f.animator.frame(0) == 0.0f);

    f.animator.apply(f.scene);
    REQUIRE(f.bladeAt() == Vec3{11.0f, 0.0f, 50.0f});
    f.animator.step(kStep, f.scene);
    REQUIRE(f.animator.frame(0) == Approx(1.0f));
    REQUIRE(f.bladeAt() == Vec3{11.0f, 0.0f, 50.0f}); // posed before moving on
    f.animator.step(kStep, f.scene);
    // A third of the way to one radian about y swings the blade off +x.
    const Vec3 blade = f.bladeAt();
    REQUIRE(blade.x == Approx(10.0f + std::cos(1.0f / 3.0f)).margin(1e-4f));
    REQUIRE(blade.z == Approx(50.0f - std::sin(1.0f / 3.0f)).margin(1e-4f));
    f.animator.step(kStep, f.scene);
    REQUIRE(f.animator.frame(0) == 0.0f); // the last frame wraps to the first
    REQUIRE_FALSE(f.animator.finished(0));
    f.animator.step(2.0f * kStep, f.scene);
    REQUIRE(f.animator.frame(0) == Approx(2.0f));

    f.animator.clear();
    REQUIRE(f.animator.size() == 0);
}

TEST_CASE("world animation jumps and loop resets cut the presentation without extra clock steps",
          "[world][animation][presentation]") {
    Fixture f("world-animator-presentation-cuts");
    f.animator.bind(f.layout);
    f.animator.apply(f.scene);
    const auto at = [&](f32 alpha) {
        f.device.draws.clear();
        f.scene.draw(f.device, Mat4{1}, CameraFrame::at(Vec3{0}), alpha);
        REQUIRE(f.device.draws.size() >= 4);
        return f.device.draws[2].vertices[0].position;
    };
    f.scene.capturePresentation();
    f.animator.fire(8, true, WorldAnimator::Finish::AtOnce);
    f.animator.apply(f.scene);
    const Vec3 endpoint = f.bladeAt();
    CHECK(at(0.0f) == endpoint);
    CHECK(at(0.5f) == endpoint);
    CHECK(f.animator.frame(0) == 3);
    CHECK(f.animator.finished(0));

    f.scene.capturePresentation();
    f.animator.hold(8);
    f.animator.apply(f.scene);
    const Vec3 start = f.bladeAt();
    CHECK(at(0.0f) == start);
    CHECK(at(0.5f) == start);
    f.animator.cycle(8, true);
    for (s32 i = 0; i < 3; ++i) {
        f.scene.capturePresentation();
        f.animator.step(kStep, f.scene);
    }
    CHECK(f.animator.frame(0) == 0);
    f.scene.capturePresentation();
    f.animator.step(kStep, f.scene);
    CHECK(at(0.0f) == start);
    CHECK(at(0.75f) == start);
    CHECK(f.animator.frame(0) == 1);
}

TEST_CASE("a held switch cycles its track and releases at the forward endpoint",
          "[world][animation]") {
    WorldScene scene;
    WorldAnimator animator;
    animator.bind(layoutWithFlags("world-animator-cycle", 0));
    animator.hold(0);
    animator.cycle(0, true);
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    CHECK_FALSE(animator.finished(0));
    animator.cycle(0, false);
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    CHECK(animator.finished(0));
    CHECK(animator.frame(0) == 3);
}

/** Exercise the actual scene presentation seam without changing the native clock or pose. */
void checkLoopPresentation(Fixture& fixture, bool continuous) {
    fixture.animator.bind(fixture.layout);
    fixture.animator.apply(fixture.scene);
    const auto& animation = fixture.layout.animations().front();
    const auto object = static_cast<usize>(animation.object);
    const auto draw = [&](f32 alpha) {
        fixture.device.draws.clear();
        fixture.scene.draw(fixture.device, Mat4{1}, CameraFrame::at(Vec3{0}), alpha);
        REQUIRE(fixture.device.draws.size() >= 4);
        return fixture.device.draws[2].vertices[0].position;
    };
    // The keys play out as authored; a continuous turn then runs on through its seam
    // (the frames its keys stop short of the full turn) before the cycle closes.
    const auto last = static_cast<f32>(animation.frames - 1);
    const auto wrappedYet = [&] {
        return std::ranges::any_of(fixture.animator.cycleEvents(),
                                   [](const auto& event) { return event.wrapped; });
    };
    s32 ticks = 0;
    while (!wrappedYet() && ticks < animation.frames + 8) {
        const f32 frame = fixture.animator.frame(0);
        fixture.scene.capturePresentation();
        fixture.animator.step(kStep, fixture.scene);
        ++ticks;
        if (frame <= last) {
            const Mat4 expected = TreePose::localMatrix(TreePose::sample(animation.track, frame),
                                                        fixture.layout.objects()[object].position);
            CHECK(fixture.scene.localTransform(object) == expected);
        }
    }
    REQUIRE(ticks >= animation.frames - 1);
    REQUIRE(fixture.animator.frame(0) < 1);
    REQUIRE(fixture.animator.cycleEvents().size() == 1);
    CHECK(fixture.animator.cycleEvents().front().wrapped);
    const f32 wrapped = fixture.animator.frame(0);
    const Vec3 previous = draw(1);
    fixture.scene.capturePresentation();
    fixture.animator.step(kStep, fixture.scene);
    CHECK(fixture.animator.frame(0) == Approx(wrapped + 1));
    const Vec3 current = draw(1);
    REQUIRE(glm::distance(previous, current) > 0.001f);
    if (continuous) {
        CHECK(glm::distance(draw(0), previous) < 0.0001f);
        CHECK(glm::distance(draw(0.5f), previous) > 0.001f);
        CHECK(glm::distance(draw(0.5f), current) > 0.001f);
    } else {
        CHECK(draw(0) == current);
        CHECK(draw(0.5f) == current);
    }
    // Explicit trigger teleports still cut even on a track eligible for loop blending.
    fixture.scene.capturePresentation();
    fixture.animator.fire(animation.object, true, WorldAnimator::Finish::AtOnce);
    fixture.animator.apply(fixture.scene);
    CHECK(draw(0) == draw(1));
    fixture.scene.capturePresentation();
    fixture.animator.hold(animation.object);
    fixture.animator.apply(fixture.scene);
    CHECK(draw(0) == draw(1));
}

TEST_CASE("full-turn world loops interpolate their seam without smoothing authored resets",
          "[world][animation][presentation][fan-loop]") {
    for (const s32 mode : {0, 1, 2, 3, 4}) {
        CAPTURE(mode);
        Fixture fixture("world-fan-loop");
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& animation = const_cast<WorldAnimation&>(fixture.layout.animations().front());
        animation.frames = 41;
        animation.track.flags = mode == 3 ? 16U : 2U;
        animation.track.frames.clear();
        animation.track.values.clear();
        for (u16 frame = 0; frame < 41; ++frame) {
            animation.track.frames.push_back(frame);
            const f32 span = mode == 2 ? 1.0f : kTwoPi;
            const f32 angle = span * static_cast<f32>(frame) / 40.0f;
            animation.track.values.push_back(TreePose::wrapAngle(mode == 1 ? -angle : angle));
        }
        if (mode == 4) {
            animation.track.values[10] = animation.track.values[8]; // not a monotonic turn
        }
        checkLoopPresentation(fixture, mode < 2);
    }
}

TEST_CASE("a continuous turn whose keys stop short of the circle runs on through the seam",
          "[world][animation][fan-loop]") {
    // Twelve keys thirty degrees apart over eleven frames: the twelfth step, back to the
    // first key, is the seam, and the cycle is twelve frames long rather than eleven.
    Fixture fixture("world-fan-seam");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& animation = const_cast<WorldAnimation&>(fixture.layout.animations().front());
    constexpr s32 kKeys = 12;
    animation.frames = kKeys;
    animation.track.flags = 2U;
    animation.track.frames.clear();
    animation.track.values.clear();
    for (u16 frame = 0; frame < kKeys; ++frame) {
        animation.track.frames.push_back(frame);
        animation.track.values.push_back(
            TreePose::wrapAngle(kTwoPi * static_cast<f32>(frame) / static_cast<f32>(kKeys)));
    }
    fixture.animator.bind(fixture.layout);
    // The blade's first vertex swings about the object at (10, 0, 50): its angle is the turn.
    const auto yawOf = [&] {
        const Vec3 blade = fixture.bladeAt();
        return std::atan2(50.0f - blade.z, blade.x - 10.0f);
    };
    // A step poses the frame it stands on, then moves on: the first poses frame 0 again.
    fixture.animator.step(kStep, fixture.scene);
    f32 previous = yawOf();
    constexpr f32 kStepAngle = kTwoPi / static_cast<f32>(kKeys);
    const auto wrapped = [&] {
        return std::ranges::any_of(fixture.animator.cycleEvents(),
                                   [](const auto& event) { return event.wrapped; });
    };
    // Every tick turns the blade one step, the seam's tick included, and the twelfth
    // tick is the one that wraps: the spin never jolts and never doubles a step.
    for (s32 tick = 2; tick <= kKeys * 2 + 1; ++tick) {
        fixture.animator.step(kStep, fixture.scene);
        const f32 now = yawOf();
        CHECK(std::abs(TreePose::wrapAngle(now - previous)) == Approx(kStepAngle).margin(1e-4f));
        previous = now;
        CHECK(wrapped() == (tick % kKeys == 0));
    }
    CHECK(fixture.animator.frame(0) == Approx(1.0f).margin(1e-4f));
    // A cursor through the seam is a valid one to restore; one past it, or one claiming
    // another seam than the track's, is not.
    auto cursors = fixture.animator.checkpoint();
    CHECK(cursors[0].seam == Approx(1.0f));
    cursors[0].frame = static_cast<f32>(kKeys - 1) + 0.5f;
    CHECK(cursors[0].valid());
    CHECK(fixture.animator.restore(cursors));
    cursors[0].frame = static_cast<f32>(kKeys - 1) + 1.5f;
    CHECK_FALSE(cursors[0].valid());
    cursors[0].frame = 0.0f;
    cursors[0].seam = 0.0f;
    CHECK(cursors[0].valid());
    CHECK_FALSE(fixture.animator.restore(cursors));
}

TEST_CASE("native Mothership full-turn fans retain interpolation across their loop seams",
          "[world][animation][presentation][fan-loop][assets]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELK1/WORLDS.PS2").parent_path();
    WorldLayout native;
    REQUIRE(native.load(directory));
    for (const auto* const name : {"K1NC_FAN", "K1NC#09", "K1NC_GROUP552", "K1NC#05"}) {
        CAPTURE(name);
        const auto object = std::ranges::find(native.objects(), name, &WorldObject::name);
        REQUIRE(object != native.objects().end());
        const auto index = static_cast<s32>(object - native.objects().begin());
        const auto found = std::ranges::find(native.animations(), index, &WorldAnimation::object);
        REQUIRE(found != native.animations().end());
        Fixture fixture("native-world-fan-loop");
        // Use the native keys on the test blade, so the regression measures presentation
        // independently of a fan's repeated blade geometry or camera framing.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& animation = const_cast<WorldAnimation&>(fixture.layout.animations().front());
        const s32 testObject = animation.object;
        animation = *found;
        animation.object = testObject;
        checkLoopPresentation(fixture, std::string_view{name} != "K1NC#05");
    }
}

TEST_CASE("one-shot animations stop at their last frame, backwards ones at their first",
          "[world][animation]") {
    WorldScene scene;
    WorldAnimator animator;
    animator.bind(layoutWithFlags("world-animator-once", WorldObject::kOnce));
    REQUIRE(animator.size() == 1);
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    REQUIRE(animator.frame(0) == 3.0f);
    REQUIRE(animator.finished(0));

    animator.bind(layoutWithFlags("world-animator-reverse", WorldObject::kReverse));
    REQUIRE(animator.frame(0) == 3.0f);
    animator.step(kStep, scene);
    REQUIRE(animator.frame(0) == Approx(2.0f));
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    REQUIRE(animator.frame(0) == 0.0f);
    REQUIRE(animator.finished(0));

    // Both flags together switch the animation off.
    animator.bind(
        layoutWithFlags("world-animator-off", WorldObject::kOnce | WorldObject::kReverse));
    REQUIRE(animator.size() == 0);
}

TEST_CASE("pausing looping world tracks does not hold one-shot trigger movement",
          "[world][animation][stop-time]") {
    WorldScene scene;
    WorldAnimator animator;
    animator.bind(layoutWithFlags("world-time-stop", 0));
    animator.step(kStep, scene);
    REQUIRE(animator.frame(0) == Approx(1));
    animator.step(5, scene, true);
    CHECK(animator.frame(0) == Approx(1));
    animator.step(kStep, scene);
    CHECK(animator.frame(0) == Approx(2));
    animator.fire(0, true);
    animator.step(1, scene, true);
    CHECK(animator.finished(0));
    CHECK(animator.frame(0) == Approx(3));
    animator.fire(0, false);
    animator.step(1, scene, true);
    CHECK(animator.finished(0));
    CHECK(animator.frame(0) == Approx(0));
}

TEST_CASE("world track loops retain time at their final-key endpoint across cadences",
          "[world][animation][cadence]") {
    const WorldLayout layout = layoutWithFlags("world-cadence", 0);
    for (const s32 rate : {30, 60, 144, 240}) {
        CAPTURE(rate);
        WorldScene scene;
        WorldAnimator animator;
        animator.bind(layout);
        for (s32 update = 0; update < rate; ++update) {
            animator.step(1.0f / static_cast<f32>(rate), scene);
        }
        REQUIRE(animator.frame(0) == Approx(0.0f).margin(0.0001f));
        // This track has keys 0..3, so its authored period is three 30 Hz intervals.
        animator.step(8.0f / 30.0f, scene);
        REQUIRE(animator.frame(0) == Approx(2.0f).margin(0.0001f));
    }
}

TEST_CASE("world animation checkpoints replay exact poses and cycle boundaries",
          "[world][animation][world-rollback]") {
    for (const s32 mode : {0, 1, 2, 3, 4}) {
        for (const s32 rate : {30, 60, 120}) {
            CAPTURE(mode, rate);
            Fixture host("world-checkpoint-host");
            Fixture replay("world-checkpoint-replay");
            host.animator.bind(host.layout);
            replay.animator.bind(replay.layout);
            if (mode == 1) {
                host.animator.fire(8, true);
            } else if (mode == 2) {
                host.animator.fire(8, true, WorldAnimator::Finish::AtOnce);
                host.animator.fire(8, false);
            } else if (mode == 3) {
                host.animator.hold(8);
            } else if (mode == 4) {
                host.animator.fire(8, true, WorldAnimator::Finish::AtOnce);
            }
            const f32 seconds = 1.0f / static_cast<f32>(rate);
            host.animator.step(seconds, host.scene);
            const auto saved = host.animator.checkpoint();
            REQUIRE(saved.size() == 1);
            REQUIRE(saved.front().valid());
            REQUIRE(replay.animator.restore(saved));
            CHECK(replay.animator.cycleEvents().empty());
            for (s32 tick = 0; tick < 30; ++tick) {
                const bool paused = tick >= 4 && tick < 8;
                host.scene.capturePresentation();
                replay.scene.capturePresentation();
                host.animator.step(seconds, host.scene, paused);
                replay.animator.step(seconds, replay.scene, paused);
                CHECK(replay.animator.checkpoint() == host.animator.checkpoint());
                CHECK(replay.bladeAt() == host.bladeAt());
                const auto expected = host.animator.cycleEvents();
                const auto actual = replay.animator.cycleEvents();
                REQUIRE(actual.size() == expected.size());
                for (usize event = 0; event < expected.size(); ++event) {
                    CHECK(actual[event].object == expected[event].object);
                    CHECK(actual[event].wrapped == expected[event].wrapped);
                }
            }
            REQUIRE(replay.animator.restore(saved));
            CHECK(replay.animator.checkpoint() == saved);
            CHECK(replay.animator.cycleEvents().empty());
        }
    }
}

TEST_CASE("world animation cursor restoration validates the whole bound roster before mutation",
          "[world][animation][world-rollback]") {
    Fixture fixture("world-checkpoint-reject");
    fixture.animator.bind(fixture.layout);
    fixture.animator.step(kStep, fixture.scene);
    const auto saved = fixture.animator.checkpoint();
    const auto eventCount = fixture.animator.cycleEvents().size();
    const auto reject = [&](const auto& changed) {
        CHECK_FALSE(fixture.animator.restore(changed));
        CHECK(fixture.animator.checkpoint() == saved);
        CHECK(fixture.animator.cycleEvents().size() == eventCount);
    };
    reject(std::vector<WorldAnimator::Cursor>{});
    auto changed = saved;
    changed.push_back(changed.front());
    reject(changed);
    for (const f32 invalid : {-1.0f, 4.0f, std::numeric_limits<f32>::infinity(),
                              std::numeric_limits<f32>::quiet_NaN()}) {
        changed = saved;
        changed.front().frame = invalid;
        reject(changed);
    }
    changed = saved;
    ++changed.front().object;
    reject(changed);
    changed = saved;
    ++changed.front().frames;
    reject(changed);
    changed = saved;
    changed.front().finished = true;
    reject(changed);
    changed = saved;
    changed.front().held = true;
    reject(changed);
    REQUIRE(fixture.animator.restore(saved));
    CHECK(fixture.animator.cycleEvents().empty());
}
} // namespace
