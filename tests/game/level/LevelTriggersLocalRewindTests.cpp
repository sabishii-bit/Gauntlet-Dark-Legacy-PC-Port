#include <array>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/NativeStateDigest.h"

#include "../../engine/world/SampleLevel.h"
#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelTriggers.h"
#include "game/level/LevelWorld.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;

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
        triggers.bindFigures(device, layout, AssetScope(items));
        triggers.setPlayerCount(2);
        REQUIRE(triggers.size() == 4);
        REQUIRE(triggers.figure(0));
        REQUIRE_FALSE(triggers.figure(2));
    }
    void step(f32 seconds, std::span<const TriggerVisitor> visitors) {
        animator.step(seconds, scene);
        triggers.update(seconds, visitors, animator, scene, &collision);
    }
    /** The triggers' native digest, the figures' archive named alike in every fixture. */
    NativeStateDigest::Digest digest() const {
        NativeStateResources resources;
        REQUIRE(resources.registerArchive(items, "stage/trigger-items"));
        return test::liveDigest(triggers, resources);
    }
    /** Which output queues hold anything now; they are left as they were. */
    struct Queued {
        bool refusals = false;
        bool lessons = false;
        bool settled = false;
        bool wakes = false;
    };
    Queued queued() {
        LevelTriggers::LocalSnapshot now;
        triggers.saveLocal(now);
        const Queued result{.refusals = !triggers.takeRefusals().empty(),
                            .lessons = !triggers.takeLessons().empty(),
                            .settled = !triggers.takeSettled().empty(),
                            .wakes = !triggers.takeWakes().empty()};
        REQUIRE(triggers.restoreLocal(now));
        return result;
    }
};

TEST_CASE("complete trigger state resumes native contacts queues and figure clocks",
          "[triggers-local-rewind]") {
    Fixture fixture("complete-trigger-phase");
    const f32 seconds = GENERATE(1.0f / 60, 1.0f / 30);
    const auto uploads = fixture.device.texturesCreated;
    bool refusals = false;
    bool wakes = false;
    bool lessons = false;
    bool settled = false;
    bool held = false;
    LevelTriggers::LocalSnapshot before;
    LevelTriggers::LocalSnapshot expected;
    LevelTriggers::LocalSnapshot current;
    for (s32 tick = 0; tick < 240; ++tick) {
        std::array<TriggerVisitor, 2> visitors{};
        visitors[0].party = 3;
        visitors[1].party = 1;
        visitors[0].height = visitors[1].height = 5;
        const s32 phase = (tick / 20) % 5;
        visitors[0].position = {static_cast<f32>(phase * 20), 0, 0};
        visitors[1].position = {phase == 3 && tick % 20 >= 10 ? 60.0f : 100.0f, 0, 0};
        visitors[0].crystals[1] = tick >= 100 ? 15 : 0;
        fixture.triggers.setCameraHeld(tick % 12 >= 6);
        if (tick % 17 == 0) {
            fixture.triggers.shoot(2);
        }
        fixture.triggers.saveLocal(before);
        const auto animator = fixture.animator;
        const auto geometry = fixture.scene.geometry();
        const auto collision = fixture.collision;
        fixture.step(seconds, visitors);
        fixture.triggers.saveLocal(expected);
        const auto queued = fixture.queued();
        refusals |= queued.refusals;
        wakes |= queued.wakes;
        lessons |= queued.lessons;
        settled |= queued.settled;
        held |= fixture.triggers.trigger(3).heldContacts == 10;
        const auto expectedAnimation = fixture.animator.checkpoint();
        std::vector<Mat4> transforms;
        for (usize object = 0; object < fixture.layout.objects().size(); ++object) {
            transforms.push_back(fixture.scene.worldTransform(object));
        }
        REQUIRE(fixture.triggers.restoreLocal(before));
        fixture.triggers.saveLocal(current);
        CHECK(fixture.triggers.equalLocal(current, before));
        // These are external owners, deliberately restored separately. A trigger
        // restore itself must not fire its commands or pose world geometry.
        CHECK(fixture.animator.checkpoint() == expectedAnimation);
        fixture.animator = animator;
        fixture.collision = collision;
        REQUIRE(fixture.scene.applyGeometry(geometry));
        fixture.step(seconds, visitors);
        fixture.triggers.saveLocal(current);
        CHECK(fixture.triggers.equalLocal(current, expected));
        CHECK(fixture.animator.checkpoint() == expectedAnimation);
        for (usize object = 0; object < transforms.size(); ++object) {
            CHECK(fixture.scene.worldTransform(object) == transforms[object]);
        }
    }
    CHECK(refusals);
    CHECK(wakes);
    CHECK(lessons);
    CHECK(settled);
    CHECK(held);
    CHECK(fixture.device.texturesCreated == uploads);
    LevelTriggers::LocalSnapshot pending;
    fixture.triggers.saveLocal(pending);
    const auto pendingRefusals = fixture.triggers.takeRefusals();
    const auto pendingLessons = fixture.triggers.takeLessons();
    const auto pendingOpenings = fixture.triggers.takeOpenings();
    const auto pendingSettled = fixture.triggers.takeSettled();
    const auto pendingCameraCues = fixture.triggers.takeCameraCues();
    const auto pendingWakes = fixture.triggers.takeWakes();
    REQUIRE(fixture.triggers.restoreLocal(pending));
    CHECK(fixture.triggers.takeRefusals() == pendingRefusals);
    CHECK(fixture.triggers.takeLessons() == pendingLessons);
    CHECK(fixture.triggers.takeOpenings() == pendingOpenings);
    CHECK(fixture.triggers.takeSettled() == pendingSettled);
    CHECK(fixture.triggers.takeCameraCues() == pendingCameraCues);
    CHECK(fixture.triggers.takeWakes() == pendingWakes);
    REQUIRE(fixture.triggers.restoreLocal(pending));
    fixture.triggers.saveLocal(current);
    CHECK(fixture.triggers.equalLocal(current, pending));
}

TEST_CASE("native campaign trigger owners retain their complete loaded roster and figures",
          "[triggers-local-rewind][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto* const name = GENERATE("G1", "A1", "J4");
    CAPTURE(name);
    const auto level = catalog.byName(name);
    REQUIRE(level);
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    WorldAnimator animator;
    animator.bind(world.layout());
    WorldCollision collision = world.collision();
    LevelTriggers owner;
    owner.bind(world.layout(), animator, &collision);
    owner.bindFigures(device, world.layout(), world.itemScope());
    REQUIRE(owner.size() > 0);
    LevelTriggers::LocalSnapshot state;
    owner.saveLocal(state);
    const auto uploads = device.texturesCreated;
    owner.setPlayerCount(4);
    owner.setCameraHeld(true);
    for (usize index = 0; index < owner.size(); ++index) {
        owner.shoot(index);
    }
    REQUIRE(owner.restoreLocal(state));
    LevelTriggers::LocalSnapshot current;
    owner.saveLocal(current);
    CHECK(owner.equalLocal(current, state));
    CHECK(device.texturesCreated == uploads);
}

TEST_CASE(
    "native trigger rewind discards a wrong branch including contacts figures and queued cues",
    "[triggers-local-rewind]") {
    Fixture reference("triggers-local-reference");
    Fixture predicted("triggers-local-predicted");
    LevelTriggers::LocalSnapshot saved;
    std::array<LevelTriggers::LocalSnapshot, 8> expectedNative;
    LevelTriggers::LocalSnapshot observed;
    WorldAnimator::LocalSnapshot savedAnimator;
    WorldScene::LocalSnapshot savedScene;
    WorldCollision::LocalSnapshot savedCollision;
    const auto uploads = predicted.device.texturesCreated;
    constexpr f32 kStep = 1.0f / 60;
    const auto advance = [&](Fixture& fixture, s32 tick) {
        std::array<TriggerVisitor, 2> visitors{};
        visitors[0].party = 3;
        visitors[1].party = 1;
        visitors[0].height = visitors[1].height = 5;
        const s32 phase = (tick / 20) % 5;
        visitors[0].position = {static_cast<f32>(phase * 20), 0, 0};
        visitors[1].position = {phase == 3 && tick % 20 >= 10 ? 60.0f : 100.0f, 0, 0};
        visitors[0].crystals[1] = tick >= 100 ? 15 : 0;
        fixture.triggers.setPlayerCount(tick % 19 == 0 ? 1 : 2);
        fixture.triggers.setCameraHeld(tick % 12 >= 6);
        if (tick % 17 == 0) {
            fixture.triggers.shoot(2);
        }
        fixture.step(kStep, visitors);
        // Keep the initial refusal until the observer has seen it. Its cooldown
        // outlasts the no-crystal branch; draining tick zero would make the
        // coverage assertion miss the only refusal this journal produces.
        if (tick > 0 && tick % 31 == 0) {
            fixture.triggers.takeRefusals();
            fixture.triggers.takeLessons();
            fixture.triggers.takeOpenings();
            fixture.triggers.takeSettled();
            fixture.triggers.takeCameraCues();
            fixture.triggers.takeWakes();
        }
    };
    s32 tick = 0;
    bool sawRefusal = false;
    bool sawWake = false;
    bool sawLesson = false;
    bool sawSettled = false;
    for (s32 window = 0; window < 96; ++window) {
        CAPTURE(window, tick);
        const s32 distance = 1 + window % 8;
        predicted.triggers.saveLocal(saved);
        predicted.animator.saveLocal(savedAnimator);
        predicted.scene.saveLocal(savedScene);
        predicted.collision.saveLocal(savedCollision);
        for (s32 step = 0; step < distance; ++step) {
            advance(predicted, tick + step);
            predicted.triggers.saveLocal(expectedNative[static_cast<usize>(step)]);
        }
        REQUIRE(predicted.triggers.restoreLocal(saved));
        REQUIRE(predicted.animator.restoreLocal(savedAnimator));
        REQUIRE(predicted.scene.restoreLocal(savedScene));
        REQUIRE(predicted.collision.restoreLocal(savedCollision));

        // A speculative branch can consume output and permanently activate a linked
        // switch. Neither may escape when corrected input replaces that branch.
        predicted.triggers.activate(8, true, predicted.animator, predicted.scene,
                                    &predicted.collision);
        predicted.triggers.setPlayerCount(0);
        predicted.triggers.setCameraHeld(true);
        for (s32 step = 0; step < distance; ++step) {
            predicted.triggers.shoot(2);
            predicted.step(kStep, {});
        }
        predicted.triggers.takeRefusals();
        predicted.triggers.takeLessons();
        predicted.triggers.takeOpenings();
        predicted.triggers.takeSettled();
        predicted.triggers.takeCameraCues();
        predicted.triggers.takeWakes();
        const auto wrongTransform = predicted.scene.worldTransform(1);
        REQUIRE(predicted.triggers.restoreLocal(saved));
        predicted.triggers.saveLocal(observed);
        CHECK(predicted.triggers.equalLocal(observed, saved));
        CHECK(predicted.scene.worldTransform(1) == wrongTransform);
        REQUIRE(predicted.animator.restoreLocal(savedAnimator));
        REQUIRE(predicted.scene.restoreLocal(savedScene));
        REQUIRE(predicted.collision.restoreLocal(savedCollision));

        for (s32 step = 0; step < distance; ++step, ++tick) {
            advance(reference, tick);
            advance(predicted, tick);
            predicted.triggers.saveLocal(observed);
            CHECK(
                predicted.triggers.equalLocal(observed, expectedNative[static_cast<usize>(step)]));
            CHECK(predicted.digest() == reference.digest());
            CHECK(predicted.animator.checkpoint() == reference.animator.checkpoint());
            const auto queued = reference.queued();
            sawRefusal |= queued.refusals;
            sawWake |= queued.wakes;
            sawLesson |= queued.lessons;
            sawSettled |= queued.settled;
            for (usize object = 0; object < reference.layout.objects().size(); ++object) {
                CHECK(predicted.scene.worldTransform(object) ==
                      reference.scene.worldTransform(object));
            }
        }
        reference.device.draws.clear();
        predicted.device.draws.clear();
        reference.triggers.draw(reference.device, Mat4{1}, {});
        predicted.triggers.draw(predicted.device, Mat4{1}, {});
        REQUIRE(predicted.device.draws.size() == reference.device.draws.size());
        for (usize draw = 0; draw < reference.device.draws.size(); ++draw) {
            CHECK(predicted.device.draws[draw].transform == reference.device.draws[draw].transform);
            CHECK(predicted.device.draws[draw].vertices == reference.device.draws[draw].vertices);
        }
        REQUIRE(predicted.triggers.canRestoreLocal(saved));
    }
    CHECK(sawRefusal);
    CHECK(sawWake);
    CHECK(sawLesson);
    CHECK(sawSettled);
    CHECK(predicted.device.texturesCreated == uploads);
}

TEST_CASE("native trigger equality retains camera holds and undelivered refusals",
          "[triggers-local-rewind]") {
    Fixture fixture("triggers-local-hidden-values");
    LevelTriggers::LocalSnapshot before;
    LevelTriggers::LocalSnapshot held;
    LevelTriggers::LocalSnapshot queued;
    LevelTriggers::LocalSnapshot drained;
    LevelTriggers::LocalSnapshot restored;
    fixture.triggers.saveLocal(before);
    const auto trigger = fixture.triggers.trigger(0);
    fixture.triggers.setCameraHeld(true);
    fixture.triggers.saveLocal(held);
    CHECK(fixture.triggers.trigger(0) == trigger);
    CHECK_FALSE(fixture.triggers.equalLocal(before, held));
    REQUIRE(fixture.triggers.restoreLocal(before));
    std::array<TriggerVisitor, 1> visitors{};
    visitors[0].party = 1;
    visitors[0].height = 5;
    fixture.step(1.0f / 60, visitors);
    fixture.triggers.saveLocal(queued);
    const auto contact = fixture.triggers.trigger(0);
    const auto refusals = fixture.triggers.takeRefusals();
    REQUIRE_FALSE(refusals.empty());
    fixture.triggers.saveLocal(drained);
    CHECK(fixture.triggers.trigger(0) == contact);
    CHECK_FALSE(fixture.triggers.equalLocal(queued, drained));
    REQUIRE(fixture.triggers.restoreLocal(queued));
    fixture.triggers.saveLocal(restored);
    CHECK(fixture.triggers.equalLocal(queued, restored));
    CHECK(fixture.triggers.takeRefusals() == refusals);
}

TEST_CASE("native trigger histories reject foreign owners and rebinding",
          "[triggers-local-rewind]") {
    Fixture first("triggers-local-guards");
    Fixture other("triggers-local-foreign");
    LevelTriggers::LocalSnapshot saved;
    CHECK_FALSE(first.triggers.restoreLocal(saved));
    CHECK_FALSE(first.triggers.equalLocal(saved, saved));
    first.triggers.saveLocal(saved);
    CHECK(first.triggers.equalLocal(saved, saved));
    LevelTriggers::LocalSnapshot otherBefore;
    other.triggers.saveLocal(otherBefore);
    CHECK_FALSE(other.triggers.restoreLocal(saved));
    CHECK_FALSE(other.triggers.equalLocal(saved, saved));
    LevelTriggers::LocalSnapshot otherAfter;
    other.triggers.saveLocal(otherAfter);
    CHECK(other.triggers.equalLocal(otherAfter, otherBefore));
    REQUIRE(first.triggers.restoreLocal(saved));
    REQUIRE(first.triggers.restoreLocal(saved));
    REQUIRE(first.triggers.restoreMotion(first.triggers.motion()));
    CHECK_FALSE(first.triggers.canRestoreLocal(saved));
    CHECK_FALSE(first.triggers.equalLocal(saved, saved));
    first.triggers.saveLocal(saved);
    first.triggers.bindFigures(first.device, first.layout, AssetScope(first.items));
    CHECK_FALSE(first.triggers.restoreLocal(saved));
    first.triggers.saveLocal(saved);
    first.triggers.bind(first.layout, first.animator, &first.collision);
    CHECK_FALSE(first.triggers.restoreLocal(saved));
    first.triggers.saveLocal(saved);
    first.triggers.clear();
    CHECK_FALSE(first.triggers.restoreLocal(saved));
    LevelTriggers empty;
    empty.saveLocal(saved);
    REQUIRE(empty.restoreLocal(saved));
}
} // namespace
