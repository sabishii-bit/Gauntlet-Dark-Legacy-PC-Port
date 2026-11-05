#include <array>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/LevelTriggers.h"

namespace {
using namespace gdl;
using namespace gdl::game;
constexpr f32 kStep = 1.0f / 60;

auto digestOf(const LevelTriggers& triggers) {
    NativeStateDigest digest;
    REQUIRE(triggers.appendNativeDigest(digest));
    return digest.finish();
}

template <class T> auto valueDigest(const T& value) {
    NativeStateDigest digest;
    digest.value(value);
    REQUIRE(digest.good());
    return digest.finish();
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items;
    WorldScene scene;
    WorldAnimator animator;
    WorldCollision collision;
    LevelTriggers triggers;

    explicit Fixture(std::string_view name) {
        const auto directory = test::sampleLevel(name);
        writeTextFile(directory / "world.json", R"({
          "objects":[{"name":"WALL","position":[0,0,0],"flags":4096},
                     {"name":"WALL","position":[20,0,0],"flags":4096},
                     {"name":"WALL","position":[40,0,0],"flags":4096},
                     {"name":"WALL","position":[60,0,0],"flags":4096}],
          "itemInfos":[{"type":5,"subtype":24,"name":"BRIDGEPAD","radius":2,"height":5},
                       {"type":5,"subtype":31,"name":"BRIDGEPAD","radius":2,"height":5}],
          "itemInstances":[
             {"info":0,"position":[0,0,0],"params":[0,0,80,0,4,2,1,0,0,0,0,0]},
             {"info":0,"position":[20,0,0],"params":[1,0,4,48,4,3,8,9,0,0,100,0]},
             {"info":1,"flags":2,"position":[40,0,0],"params":[2,0,2,0,4,255,9,0,0,0,50,0]},
             {"info":0,"position":[60,0,0],"params":[3,0,0,4,4,4,7,0,0,0,50,0]}]})");
        writeTextFile(directory / "animations.json", R"({"trees":[{"name":"BRIDGEPAD",
          "nodes":[{"name":"WALL","object":"WALL","parent":-1,"position":[0,0,0]}],
          "sequences":[{"name":"OFF","frames":10,"rate":30},
                       {"name":"DOWN","frames":4,"rate":30},
                       {"name":"ON","frames":10,"rate":30},
                       {"name":"UP","frames":4,"rate":30}]}]})");
        REQUIRE(layout.load(directory));
        REQUIRE(items.load(directory));
        REQUIRE(scene.build(layout, items.models, items.textures, device));
        animator.bind(layout);
        triggers.bind(layout, animator, &collision);
        triggers.setPlayerCount(2);
        REQUIRE(triggers.size() == 4);
        // This component fixture has no art binding resolver. Composite stage
        // tests cover figures with their resource catalog; no pointer is hashed.
        REQUIRE_FALSE(triggers.figure(0));
        REQUIRE_FALSE(triggers.figure(2));
    }
    void advance(s32 tick) {
        std::array<TriggerVisitor, 2> visitors{};
        visitors[0].party = 3;
        visitors[1].party = 1;
        const s32 phase = (tick / 20) % 5;
        visitors[0].position = {static_cast<f32>(phase * 20), 0, 0};
        visitors[1].position = {phase == 3 && tick % 20 >= 10 ? 60.0f : 100.0f, 0, 0};
        visitors[0].crystals[1] = tick >= 100 ? 15 : 0;
        triggers.setPlayerCount(tick % 19 == 0 ? 1 : 2);
        triggers.setCameraHeld(tick % 12 >= 6);
        if (tick % 17 == 0) {
            triggers.shoot(2);
        }
        animator.step(kStep, scene);
        triggers.update(kStep, visitors, animator, scene, &collision);
    }
};

TEST_CASE(
    "native trigger digests match independent owners and corrected one to eight tick branches",
    "[native-state-digest][triggers-native-digest][triggers-local-rewind]") {
    Fixture reference("trigger-native-digest-reference");
    Fixture predicted("trigger-native-digest-predicted");
    CHECK(digestOf(reference.triggers) == digestOf(predicted.triggers));
    LevelTriggers::LocalSnapshot saved;
    WorldAnimator::LocalSnapshot animator;
    WorldScene::LocalSnapshot scene;
    WorldCollision::LocalSnapshot collision;
    s32 tick = 0;
    for (s32 window = 0; window < 32; ++window) {
        const s32 distance = window % 8 + 1;
        CAPTURE(window, tick, distance);
        predicted.triggers.saveLocal(saved);
        predicted.animator.saveLocal(animator);
        predicted.scene.saveLocal(scene);
        predicted.collision.saveLocal(collision);
        const auto before = digestOf(predicted.triggers);
        NativeStateDigest snapshot;
        REQUIRE(predicted.triggers.appendLocalDigest(snapshot, saved));
        CHECK(snapshot.finish() == before);
        predicted.triggers.setPlayerCount(0);
        predicted.triggers.setCameraHeld(true);
        predicted.triggers.activate(8, WorldAnimator::Finish::AtOnce, predicted.animator,
                                    predicted.scene, &predicted.collision);
        predicted.triggers.takeOpenings();
        for (s32 step = 0; step < distance; ++step) {
            predicted.triggers.shoot(2);
            predicted.animator.step(kStep, predicted.scene);
            predicted.triggers.update(kStep, {}, predicted.animator, predicted.scene,
                                      &predicted.collision);
        }
        CHECK(digestOf(predicted.triggers) != before);
        NativeStateDigest retained;
        REQUIRE(predicted.triggers.appendLocalDigest(retained, saved));
        CHECK(retained.finish() == before);
        REQUIRE(predicted.triggers.restoreLocal(saved));
        REQUIRE(predicted.animator.restoreLocal(animator));
        REQUIRE(predicted.scene.restoreLocal(scene));
        REQUIRE(predicted.collision.restoreLocal(collision));
        CHECK(digestOf(predicted.triggers) == before);
        for (s32 step = 0; step < distance; ++step, ++tick) {
            reference.advance(tick);
            predicted.advance(tick);
            CHECK(digestOf(predicted.triggers) == digestOf(reference.triggers));
        }
    }
}

TEST_CASE("native trigger digest includes held contacts timers eligibility and undelivered cues",
          "[native-state-digest][triggers-native-digest]") {
    Fixture fixture("trigger-native-digest-hidden");
    LevelTriggers::LocalSnapshot initial;
    fixture.triggers.saveLocal(initial);
    const auto before = digestOf(fixture.triggers);
    fixture.triggers.setCameraHeld(true);
    CHECK(digestOf(fixture.triggers) != before);
    REQUIRE(fixture.triggers.restoreLocal(initial));
    fixture.triggers.setPlayerCount(3);
    CHECK(digestOf(fixture.triggers) != before);
    REQUIRE(fixture.triggers.restoreLocal(initial));
    fixture.triggers.shoot(2);
    CHECK(digestOf(fixture.triggers) != before);
    REQUIRE(fixture.triggers.restoreLocal(initial));
    fixture.triggers.update(0.001f, {}, fixture.animator, fixture.scene, &fixture.collision);
    CHECK(digestOf(fixture.triggers) != before); // Sub-frame native clock, not a motion frame.
    REQUIRE(fixture.triggers.restoreLocal(initial));
    std::array<TriggerVisitor, 2> away{};
    for (auto& visitor : away) {
        visitor.position = Vec3{1000, 0, 0};
    }
    fixture.triggers.update(0, away, fixture.animator, fixture.scene, &fixture.collision);
    const auto twoVisitors = digestOf(fixture.triggers);
    REQUIRE(fixture.triggers.restoreLocal(initial));
    fixture.triggers.update(0, std::span{away}.first(1), fixture.animator, fixture.scene,
                            &fixture.collision);
    CHECK(digestOf(fixture.triggers) != twoVisitors); // Empty-toggle delay depends on party size.
    REQUIRE(fixture.triggers.restoreLocal(initial));

    std::array<bool, 6> witnessed{};
    for (s32 tick = 0; tick < 240; ++tick) {
        fixture.advance(tick);
        const auto drain = [&](usize kind, auto&& consume) {
            const auto queued = digestOf(fixture.triggers);
            if (!consume().empty()) {
                witnessed[kind] = true;
                CHECK(digestOf(fixture.triggers) != queued);
            }
        };
        drain(0, [&] { return fixture.triggers.takeRefusals(); });
        drain(1, [&] { return fixture.triggers.takeLessons(); });
        drain(2, [&] { return fixture.triggers.takeOpenings(); });
        drain(3, [&] { return fixture.triggers.takeSettled(); });
        drain(4, [&] { return fixture.triggers.takeCameraCues(); });
        drain(5, [&] { return fixture.triggers.takeWakes(); });
    }
    for (usize kind = 0; kind < witnessed.size(); ++kind) {
        CAPTURE(kind);
        CHECK(witnessed[kind]);
    }
}

TEST_CASE("native trigger records hash every native contact and target value",
          "[native-state-digest][triggers-native-digest]") {
    LevelTrigger trigger;
    const auto changedTrigger = [&](auto&& change) {
        auto changed = trigger;
        change(changed);
        CHECK(valueDigest(changed) != valueDigest(trigger));
    };
    changedTrigger([](auto& v) { ++v.instance; });
    changedTrigger([](auto& v) { v.spot.x = 1; });
    changedTrigger([](auto& v) { ++v.target; });
    changedTrigger([](auto& v) { ++v.id; });
    changedTrigger([](auto& v) { ++v.nextId; });
    changedTrigger([](auto& v) { v.refusalCooldown = 0.001f; });
    changedTrigger([](auto& v) { v.toggleCooldown = 0.001f; });
    changedTrigger([](auto& v) { ++v.next; });
    changedTrigger([](auto& v) { v.chained = true; });
    changedTrigger([](auto& v) { v.flags = 1; });
    changedTrigger([](auto& v) { v.kind = 1; });
    changedTrigger([](auto& v) { v.radius = 1; });
    changedTrigger([](auto& v) { ++v.sound; });
    changedTrigger([](auto& v) { v.fired = true; });
    changedTrigger([](auto& v) { v.shootable = true; });
    changedTrigger([](auto& v) { v.height = 1; });
    changedTrigger([](auto& v) { v.shot = true; });
    changedTrigger([](auto& v) { v.occupied = true; });
    changedTrigger([](auto& v) { v.movementLesson = true; });
    changedTrigger([](auto& v) { v.forced = true; });
    changedTrigger([](auto& v) { v.minPlayers = 2; });
    changedTrigger([](auto& v) { v.enabled = false; });
    changedTrigger([](auto& v) { v.heldContacts = 8; });
    changedTrigger([](auto& v) { ++v.floor; });
    changedTrigger([](auto& v) { v.localPlacement[3].y = 1; });
    changedTrigger([](auto& v) { v.placement[3].z = 1; });

    LevelTriggers::Target target;
    const auto changedTarget = [&](auto&& change) {
        auto changed = target;
        change(changed);
        CHECK(valueDigest(changed) != valueDigest(target));
    };
    changedTarget([](auto& v) { ++v.subtype; });
    changedTarget([](auto& v) { ++v.object; });
    changedTarget([](auto& v) { v.kind = 1; });
    changedTarget([](auto& v) { v.animated = true; });
    changedTarget([](auto& v) { v.open = true; });
    changedTarget([](auto& v) { v.settled = false; });
    changedTarget([](auto& v) { v.pressed = true; });
    changedTarget([](auto& v) { v.wholePartyReady = true; });
    changedTarget([](auto& v) { v.returning = true; });
    changedTarget([](auto& v) { v.forced = true; });
    changedTarget([](auto& v) { v.alpha = 0.5f; });
    changedTarget([](auto& v) { v.origin.x = 1; });
    changedTarget([](auto& v) { v.height = 1; });
    changedTarget([](auto& v) { v.closedHeight = 1; });
    changedTarget([](auto& v) { v.openHeight = 1; });
    changedTarget([](auto& v) { v.subtree.push_back(3); });
    changedTarget([](auto& v) { v.spot.z = 1; });
    changedTarget([](auto& v) { ++v.sound; });
}

TEST_CASE("native trigger snapshot digest rejects foreign expired and rebound owners atomically",
          "[native-state-digest][triggers-native-digest]") {
    LevelTriggers a;
    const LevelTriggers b{};
    LevelTriggers::LocalSnapshot saved;
    NativeStateDigest digest;
    digest.string("prefix");
    const auto before = digest.finish();
    CHECK_FALSE(a.appendLocalDigest(digest, saved));
    a.saveLocal(saved);
    CHECK_FALSE(b.appendLocalDigest(digest, saved));
    CHECK(digest.finish() == before);
    REQUIRE(a.appendLocalDigest(digest, saved));
    a.clear();
    const auto after = digest.finish();
    CHECK_FALSE(a.appendLocalDigest(digest, saved));
    CHECK(digest.finish() == after);
    {
        const LevelTriggers temporary;
        temporary.saveLocal(saved);
    }
    CHECK_FALSE(a.appendLocalDigest(digest, saved));
    CHECK(digest.finish() == after);
}

TEST_CASE("native trigger digest bytes stay pinned for distinct held and shot values",
          "[native-state-digest][triggers-native-digest][native-digest-pin]") {
    Fixture fixture("trigger-native-digest-pin");
    fixture.triggers.setPlayerCount(3);
    fixture.triggers.setCameraHeld(true);
    fixture.triggers.shoot(2);
    // SHA-256 of the native digest of the triggers above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "c3d01f20fdda3ad2bcfa5f843671b87eba2cb51e9b683ca2d35d1f742ec16ff9";
    CHECK(test::hexBytes(digestOf(fixture.triggers)) == kDigest);
}
} // namespace
