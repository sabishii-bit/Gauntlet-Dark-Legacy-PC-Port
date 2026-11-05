#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "SampleLevel.h"
#include "TestSupport.h"

namespace {
using namespace gdl;

/** A lent texture's name in a digest: its slot past this. The level's own keep their slot. */
constexpr u32 kLenderNames = 100;
constexpr u32 kWhiteName = 200; ///< the renderer's white texture in a digest

TextureAnimationInfo cycle(std::string_view name, s32 texture, s32 source, s32 frames, s32 rate,
                           std::string_view frameName = "") {
    TextureAnimationInfo info;
    info.name = name;
    info.frameName = frameName;
    info.texture = texture;
    info.source = source;
    info.frames = frames;
    info.rate = rate;
    return info;
}

/** The slot `bank` holds `texture` in, among the textures it has made. */
std::optional<u32> slotOf(const TextureSet& bank, const void* texture) {
    for (u32 slot = 0; slot < bank.size(); ++slot) {
        if (bank.cachedTexture(slot) == texture) {
            return slot;
        }
    }
    return std::nullopt;
}

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    TextureSet lender;
    WorldLayout layout;
    WorldScene scene;
    WorldCollision collision;
    WorldAnimator animator;
    TextureAnimator textureClock;

    /** The scene's, collision's and animator's native values, saved together. */
    struct Saved {
        WorldScene::LocalSnapshot scene;
        WorldCollision::LocalSnapshot collision;
        WorldAnimator::LocalSnapshot animator;
    };

    explicit Fixture(std::string_view name) {
        const auto dir = test::sampleLevel(name);
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(lender.load(test::sampleLender(std::string(name) + "-lender")));
        REQUIRE(layout.load(dir));
        const std::array<TextureSet*, 1> lenders{&lender};
        REQUIRE(scene.build(layout, models, textures, device, {}, lenders));
        animator.bind(layout);
        animator.apply(scene);
        std::vector<CollisionTriangle> triangles{
            {{0, 1, 0}, {Vec3{-2, 0, -2}, Vec3{2, 0, -2}, Vec3{0, 0, 2}}, 8, 4},
            {{0, 1, 0}, {Vec3{-2, 0, -2}, Vec3{2, 0, -2}, Vec3{0, 0, 2}}, 1000, 4}};
        collision.build(std::move(triangles));
        const std::array<s32, 1> moving{8};
        collision.setMovingObjects(moving);
        collision.setObjectTransform(8, scene.worldTransform(8));
        const std::array animations{
            cycle("TORCHB", 3, TextureAnimationInfo::kByName, 2, 2, "TORCH00"),
            cycle("STONE", 0, 1, 2, 1), cycle("GLASS", 1, TextureAnimationInfo::kScrollU, -4, 0)};
        textureClock.bind(animations, textures, device, lenders);
        textureClock.apply(scene);
    }

    void step(f32 seconds, u32 tick) {
        scene.capturePresentation();
        animator.step(seconds, scene);
        collision.setObjectTransform(8, scene.worldTransform(8));
        textureClock.advance(seconds);
        textureClock.apply(scene);
        scene.setObjectAlpha(9, static_cast<f32>(tick % 10) / 10);
        REQUIRE(scene.setObjectVisible(11, tick % 3 != 0));
        scene.setDarken(static_cast<f32>(tick % 5) / 10);
        const std::array<PointLight, 1> lights{
            {{{10, 1, 50}, {1, 0.5f, 0.25f}, 4, static_cast<f32>(tick % 7)}}};
        scene.setPointLights(lights);
        collision.setSolid(1000, tick % 2 == 0);
        collision.setContactOnly(1000, tick % 3 == 0);
        collision.setFloorExitBlocked(1000, tick % 5 != 0);
    }

    /** Names each texture by its bank and slot, so that two loads of the level hash alike. */
    NativeStateDigest::Resources textureNames() {
        return {this, [](NativeStateDigest& digest, std::type_index type, const void* pointer,
                         void* context) {
                    if (type != typeid(Texture)) {
                        return false;
                    }
                    const auto& fixture = *static_cast<const Fixture*>(context);
                    if (const auto slot = slotOf(fixture.textures, pointer)) {
                        digest.word(*slot);
                        return true;
                    }
                    if (const auto slot = slotOf(fixture.lender, pointer)) {
                        digest.word(kLenderNames + *slot);
                        return true;
                    }
                    if (pointer == &fixture.device.whiteTexture()) {
                        digest.word(kWhiteName);
                        return true;
                    }
                    return false;
                }};
    }
    /** The scene's, collision's and animator's native digests, for comparing two loads. */
    std::array<NativeStateDigest::Digest, 3> digests() {
        return {test::liveDigest(scene, textureNames()), test::liveDigest(collision),
                test::liveDigest(animator)};
    }

    Saved save() const {
        Saved saved;
        scene.saveLocal(saved.scene);
        collision.saveLocal(saved.collision);
        animator.saveLocal(saved.animator);
        return saved;
    }
    bool restore(const Saved& saved) {
        return scene.restoreLocal(saved.scene) && collision.restoreLocal(saved.collision) &&
               animator.restoreLocal(saved.animator);
    }
    /** Whether the three owners hold exactly what `saved` does. */
    bool holds(const Saved& saved) const {
        const Saved now = save();
        return scene.equalLocal(now.scene, saved.scene) &&
               collision.equalLocal(now.collision, saved.collision) &&
               animator.equalLocal(now.animator, saved.animator);
    }
};

void sameDraw(Fixture& first, Fixture& second, f32 alpha) {
    first.device.draws.clear();
    second.device.draws.clear();
    const CameraFrame camera = CameraFrame::at(Vec3{10, 4, 0});
    first.scene.draw(first.device, Mat4{1}, camera, alpha, -0.25f);
    second.scene.draw(second.device, Mat4{1}, camera, alpha, -0.25f);
    REQUIRE(first.device.draws.size() == second.device.draws.size());
    for (usize i = 0; i < first.device.draws.size(); ++i) {
        const auto& a = first.device.draws[i];
        const auto& b = second.device.draws[i];
        CHECK(a.transform == b.transform);
        CHECK(a.state.uvOffset == b.state.uvOffset);
        CHECK(a.state.darken == b.state.darken);
        REQUIRE(a.vertices.size() == b.vertices.size());
        for (usize j = 0; j < a.vertices.size(); ++j) {
            CHECK(a.vertices[j].position == b.vertices[j].position);
            CHECK(a.vertices[j].color == b.vertices[j].color);
        }
    }
}

TEST_CASE("native world history restores collision animation masks and rendered geometry together",
          "[local-rewind][world-local-rewind]") {
    Fixture reference("world-local-reference");
    Fixture replay("world-local-replay");
    struct Frame {
        WorldScene::LocalSnapshot scene;
        WorldCollision::LocalSnapshot collision;
        WorldAnimator::LocalSnapshot animator;
        TextureAnimator::Clock textures;
    };
    std::array<Frame, 16> history;
    std::array<Frame, 8> expected;
    Frame observed;
    const auto step = [](Fixture& pool, u32 tick, bool wrong) {
        if (tick % 11 == 0 || wrong) {
            pool.animator.fire(
                8, !wrong, wrong ? WorldAnimator::Finish::AtOnce : WorldAnimator::Finish::Animated);
        }
        if (tick % 7 == 0) {
            pool.animator.cycle(8, !wrong);
        }
        pool.step(1.0f / 60, wrong ? tick + 9 : tick);
        if (tick % 13 == 0 || wrong) {
            const Mat4 transform =
                glm::scale(glm::translate(Mat4{1}, Vec3{0, wrong ? -30.0f : 4.0f, 50}),
                           Vec3{wrong ? 0.0f : 1.0f});
            pool.scene.setObjectTransform(8, transform, true);
            pool.collision.setObjectTransform(8, pool.scene.worldTransform(8));
        }
    };
    u32 tick = 0;
    for (u32 window = 0; window < 128; ++window) {
        CAPTURE(window, tick);
        auto& saved = history[window % history.size()];
        replay.scene.saveLocal(saved.scene);
        replay.collision.saveLocal(saved.collision);
        replay.animator.saveLocal(saved.animator);
        saved.textures = replay.textureClock.clock();
        const u32 distance = window % 8 + 1;
        // Compare native values under the same binding, not resource-normalized DTOs.
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, false);
            replay.scene.saveLocal(expected[i].scene);
            replay.collision.saveLocal(expected[i].collision);
            replay.animator.saveLocal(expected[i].animator);
        }
        REQUIRE(replay.scene.restoreLocal(saved.scene));
        REQUIRE(replay.collision.restoreLocal(saved.collision));
        REQUIRE(replay.animator.restoreLocal(saved.animator));
        replay.textureClock.restoreClock(saved.textures);
        for (u32 i = 0; i < distance; ++i) {
            step(replay, tick + i, true);
        }
        // A draw must not leave stale composed transforms after correction.
        replay.scene.draw(replay.device, Mat4{1}, CameraFrame{}, 0.5f);
        REQUIRE(replay.scene.canRestoreLocal(saved.scene));
        REQUIRE(replay.collision.canRestoreLocal(saved.collision));
        REQUIRE(replay.animator.canRestoreLocal(saved.animator));
        REQUIRE(replay.scene.restoreLocal(saved.scene));
        REQUIRE(replay.collision.restoreLocal(saved.collision));
        REQUIRE(replay.animator.restoreLocal(saved.animator));
        replay.textureClock.restoreClock(saved.textures);
        CHECK(replay.digests() == reference.digests());
        sameDraw(reference, replay, 0.37f);
        for (u32 i = 0; i < distance; ++i) {
            step(reference, tick + i, false);
            step(replay, tick + i, false);
            replay.scene.saveLocal(observed.scene);
            replay.collision.saveLocal(observed.collision);
            replay.animator.saveLocal(observed.animator);
            CHECK(replay.scene.equalLocal(observed.scene, expected[i].scene));
            CHECK(replay.collision.equalLocal(observed.collision, expected[i].collision));
            CHECK(replay.animator.equalLocal(observed.animator, expected[i].animator));
            CHECK(replay.digests() == reference.digests());
            sameDraw(reference, replay, 0.37f);
            const Vec3 from{10, 4, 45};
            const Vec3 to{10, 4, 55};
            CHECK(replay.collision.sweepWalls(from, to, 1, 3, 9) ==
                  reference.collision.sweepWalls(from, to, 1, 3, 9));
        }
        tick += distance;
    }
    CHECK(replay.device.textureUpdates == 0);
}

TEST_CASE("native world history rejects foreign owners and changed geometry bindings",
          "[local-rewind][world-local-rewind]") {
    Fixture a("world-local-owner");
    Fixture b("world-local-other");
    WorldScene::LocalSnapshot scene;
    WorldCollision::LocalSnapshot collision;
    WorldAnimator::LocalSnapshot animator;
    CHECK_FALSE(a.scene.restoreLocal(scene));
    CHECK_FALSE(a.collision.restoreLocal(collision));
    CHECK_FALSE(a.animator.restoreLocal(animator));
    CHECK_FALSE(a.scene.equalLocal(scene, scene));
    CHECK_FALSE(a.collision.equalLocal(collision, collision));
    CHECK_FALSE(a.animator.equalLocal(animator, animator));
    a.scene.saveLocal(scene);
    a.collision.saveLocal(collision);
    a.animator.saveLocal(animator);
    CHECK(a.scene.equalLocal(scene, scene));
    CHECK(a.collision.equalLocal(collision, collision));
    CHECK(a.animator.equalLocal(animator, animator));
    CHECK_FALSE(b.scene.equalLocal(scene, scene));
    CHECK_FALSE(b.collision.equalLocal(collision, collision));
    CHECK_FALSE(b.animator.equalLocal(animator, animator));
    CHECK_FALSE(b.scene.restoreLocal(scene));
    CHECK_FALSE(b.collision.restoreLocal(collision));
    CHECK_FALSE(b.animator.restoreLocal(animator));
    a.collision.append(std::array{CollisionTriangle{.object = 9999}});
    CHECK_FALSE(a.collision.restoreLocal(collision));
    a.collision.saveLocal(collision);
    a.collision.setMovingObjects(std::array<s32, 1>{9999});
    CHECK_FALSE(a.collision.restoreLocal(collision));
    a.scene.clear();
    a.animator.clear();
    CHECK_FALSE(a.scene.equalLocal(scene, scene));
    CHECK_FALSE(a.animator.equalLocal(animator, animator));
    CHECK_FALSE(a.scene.restoreLocal(scene));
    CHECK_FALSE(a.animator.restoreLocal(animator));
}

TEST_CASE("native world equality sees latent texture phases query masks and held animation",
          "[local-rewind][world-local-rewind]") {
    Fixture fixture("world-local-hidden-values");
    WorldScene::LocalSnapshot sceneBefore;
    WorldScene::LocalSnapshot sceneAfter;
    WorldScene::LocalSnapshot sceneRestored;
    WorldCollision::LocalSnapshot collisionBefore;
    WorldCollision::LocalSnapshot collisionAfter;
    WorldCollision::LocalSnapshot collisionRestored;
    WorldAnimator::LocalSnapshot animatorBefore;
    WorldAnimator::LocalSnapshot animatorAfter;
    WorldAnimator::LocalSnapshot animatorRestored;
    fixture.scene.saveLocal(sceneBefore);
    fixture.collision.saveLocal(collisionBefore);
    fixture.animator.saveLocal(animatorBefore);
    const auto transform = fixture.scene.worldTransform(8);
    const auto offset = fixture.scene.textureOffset(1);
    fixture.scene.setTextureScroll(1, offset, {0.25f, 0.5f}, {0.01f, 0.02f});
    fixture.scene.saveLocal(sceneAfter);
    CHECK(fixture.scene.textureOffset(1) == offset);
    CHECK_FALSE(fixture.scene.equalLocal(sceneBefore, sceneAfter));
    fixture.collision.setContactOnly(1000, true);
    fixture.collision.setFloorExitBlocked(1000, true);
    fixture.collision.saveLocal(collisionAfter);
    CHECK(fixture.collision.solid(1000));
    CHECK_FALSE(fixture.collision.equalLocal(collisionBefore, collisionAfter));
    fixture.animator.hold(8);
    fixture.animator.saveLocal(animatorAfter);
    CHECK(fixture.scene.worldTransform(8) == transform);
    CHECK_FALSE(fixture.animator.equalLocal(animatorBefore, animatorAfter));
    REQUIRE(fixture.scene.restoreLocal(sceneBefore));
    REQUIRE(fixture.collision.restoreLocal(collisionBefore));
    REQUIRE(fixture.animator.restoreLocal(animatorBefore));
    fixture.scene.saveLocal(sceneRestored);
    fixture.collision.saveLocal(collisionRestored);
    fixture.animator.saveLocal(animatorRestored);
    CHECK(fixture.scene.equalLocal(sceneBefore, sceneRestored));
    CHECK(fixture.collision.equalLocal(collisionBefore, collisionRestored));
    CHECK(fixture.animator.equalLocal(animatorBefore, animatorRestored));
}

TEST_CASE("native level geometry reports reusable local history costs",
          "[.][assets][local-rewind-timing][world-local-rewind-timing]") {
    const auto* const level = GENERATE("G2", "G4", "L1");
    const auto directory =
        test::assetOrSkip(std::string{"LEVELS/LEVEL"} + level + "/WORLDS.PS2").parent_path();
    test::FakeRenderDevice device;
    device.recordDraws = false;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    WorldCollision collision;
    WorldAnimator animator;
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    REQUIRE(layout.load(directory));
    REQUIRE(scene.build(layout, models, textures, device));
    REQUIRE(collision.load(directory, layout));
    animator.bind(layout);
    animator.apply(scene);
    std::vector<s32> moving;
    for (usize i = 0; i < layout.objects().size(); ++i) {
        if (scene.moving(i)) {
            moving.push_back(static_cast<s32>(i));
        }
    }
    collision.setMovingObjects(moving);
    std::cout << level << "_geometry_objects=" << layout.objects().size()
              << " moving_placements=" << moving.size()
              << " static_placements=" << layout.objects().size() - moving.size() << '\n';
    const auto step = [&] {
        scene.capturePresentation();
        animator.step(1.0f / 60, scene);
        for (const s32 object : moving) {
            collision.setObjectTransform(object, scene.worldTransform(static_cast<usize>(object)));
        }
    };
    step();
    struct Frame {
        WorldScene::LocalSnapshot scene;
        WorldCollision::LocalSnapshot collision;
        WorldAnimator::LocalSnapshot animator;
    };
    std::array<Frame, 16> history;
    const auto save = [&](Frame& frame) {
        scene.saveLocal(frame.scene);
        collision.saveLocal(frame.collision);
        animator.saveLocal(frame.animator);
    };
    for (auto& frame : history) {
        save(frame);
    }
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](auto start) {
        return std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
    };
    std::array<f64, 256> saves{};
    std::array<f64, 256> restores{};
    std::array<f64, 256> replays{};
    for (usize i = 0; i < saves.size(); ++i) {
        auto& frame = history[i % history.size()];
        auto start = Clock::now();
        save(frame);
        saves[i] = elapsed(start);
        for (usize j = 0; j < 8; ++j) {
            step();
        }
        start = Clock::now();
        const bool restored = scene.restoreLocal(frame.scene) &&
                              collision.restoreLocal(frame.collision) &&
                              animator.restoreLocal(frame.animator);
        restores[i] = elapsed(start);
        REQUIRE(restored);
        start = Clock::now();
        for (usize j = 0; j < 8; ++j) {
            step();
        }
        replays[i] = elapsed(start);
    }
    const auto report = [&](std::string_view operation, auto values) {
        std::ranges::sort(values);
        std::cout << level << "_geometry_" << operation << " mean_ms="
                  << std::accumulate(values.begin(), values.end(), 0.0) / values.size()
                  << " p99_ms=" << values[values.size() * 99 / 100] << " max_ms=" << values.back()
                  << '\n';
    };
    report("save", saves);
    report("restore", restores);
    report("replay8", replays);
}

TEST_CASE("world rewind preserves native clocks, preceding poses and appended collision",
          "[world][world-local-rewind]") {
    const u32 rate = GENERATE(30u, 60u);
    Fixture authority("world-core-authority");
    Fixture replay("world-core-replay");
    const f32 seconds = 1.0f / static_cast<f32>(rate);
    const auto texturesBefore = replay.device.texturesCreated;
    for (u32 tick = 1; tick <= 7; ++tick) {
        authority.step(seconds, tick);
        replay.step(seconds, tick);
    }
    const auto baseline = authority.digests();
    const auto clock = authority.textureClock.checkpoint();
    const auto authoritySaved = authority.save();
    const auto saved = replay.save();
    // The replay leaves the authority by another branch, then rewinds to where they agreed.
    for (u32 tick = 30; tick < 34; ++tick) {
        replay.step(seconds * 2, tick);
    }
    REQUIRE(replay.digests() != baseline);
    REQUIRE(replay.restore(saved));
    REQUIRE(replay.textureClock.restore(clock));
    CHECK(replay.digests() == baseline);
    CHECK(replay.device.texturesCreated == texturesBefore);
    CHECK(replay.scene.textureOf(3) != authority.scene.textureOf(3));
    sameDraw(authority, replay, 0.0f);
    sameDraw(authority, replay, 0.5f);
    sameDraw(authority, replay, 1.0f);
    CHECK(replay.digests() == baseline); // Draw caches are not simulation changes.
    for (u32 tick = 8; tick <= rate * 2; ++tick) {
        CAPTURE(rate, tick);
        authority.step(seconds, tick);
        replay.step(seconds, tick);
        CHECK(replay.digests() == authority.digests());
        CHECK(replay.textureClock.checkpoint() == authority.textureClock.checkpoint());
    }
    REQUIRE(authority.restore(authoritySaved));
    REQUIRE(authority.textureClock.restore(clock));
    REQUIRE(replay.restore(saved));
    REQUIRE(replay.textureClock.restore(clock));
    CHECK(authority.digests() == baseline);
    CHECK(replay.digests() == baseline);
    CHECK(replay.device.texturesCreated == texturesBefore);
}

TEST_CASE("world rewind retains cycle outputs and raw masks without redispatch",
          "[world][world-local-rewind]") {
    Fixture fixture("world-core-events");
    fixture.step(3.0f / 30.0f, 3);
    REQUIRE_FALSE(fixture.animator.cycleEvents().empty());
    fixture.collision.setSolid(1000, false);
    fixture.collision.setContactOnly(1000, true);
    fixture.collision.setFloorExitBlocked(1000, true);
    const auto baseline = fixture.save();
    const auto events = fixture.animator.cycleEvents().size();
    CHECK_FALSE(fixture.collision.floorExitBlocked(1000)); // Hidden masks the public query.
    fixture.step(1.0f / 30.0f, 10);
    REQUIRE_FALSE(fixture.holds(baseline));
    REQUIRE(fixture.restore(baseline));
    CHECK(fixture.holds(baseline));
    CHECK(fixture.animator.cycleEvents().size() == events);
    fixture.collision.setSolid(1000, true);
    CHECK(fixture.collision.floorExitBlocked(1000));
    CHECK(fixture.collision.contactOnly(1000));
}

TEST_CASE("moving collision caches match native floor ray and cylinder queries after rewind",
          "[world][world-local-rewind][world-collapsed-collision]") {
    Fixture native("world-core-borrowed-native");
    Fixture replay("world-core-borrowed-replay");
    for (auto* fixture : {&native, &replay}) {
        fixture->collision.build(
            {{{0, 1, 0}, {Vec3{-2, 0, -2}, Vec3{2, 0, -2}, Vec3{0, 0, 2}}, 8, 4}});
        // Appended item collision retains its own non-layout object identity.
        const std::array appended{
            CollisionTriangle{{0, 0, 1}, {Vec3{-2, -2, 0}, Vec3{2, -2, 0}, Vec3{0, 2, 0}}, 1000, 2},
            CollisionTriangle{
                {0, 1, 0}, {Vec3{48, -3, -2}, Vec3{52, -3, -2}, Vec3{50, -3, 2}}, 2000, 4}};
        fixture->collision.append(appended);
        fixture->collision.setMovingObjects(std::array<s32, 2>{8, 1000});
    }
    WorldCollision::LocalSnapshot initial;
    replay.collision.saveLocal(initial);
    const Mat4 placed = glm::translate(Mat4{1}, Vec3{7, 3, 9}) *
                        glm::rotate(Mat4{1}, 0.4f, Vec3{0, 1, 0}) *
                        glm::scale(Mat4{1}, Vec3{1.5f, 0.75f, 2});
    const Mat4 collapsed = glm::translate(Mat4{1}, Vec3{7, 3, 9}) * glm::scale(Mat4{1}, Vec3{0});
    const std::array transforms{Mat4{1}, placed, collapsed, placed, Mat4{1}};
    const auto uploads = replay.device.texturesCreated;
    for (usize step = 0; step < transforms.size(); ++step) {
        CAPTURE(step);
        const auto& transform = transforms[step];
        for (const s32 object : {8, 1000}) {
            native.collision.setObjectTransform(object, transform);
            replay.collision.setObjectTransform(object, transform);
        }
        WorldCollision::LocalSnapshot saved;
        replay.collision.saveLocal(saved);
        // A native update after the save replaces the derived cache; the rewind must bring
        // back the saved one without changing the borrowed immutable locals.
        for (const s32 object : {8, 1000}) {
            replay.collision.setObjectTransform(object, glm::translate(Mat4{1}, Vec3{-7, 6, -9}));
        }
        REQUIRE(test::liveDigest(replay.collision) != test::liveDigest(native.collision));
        REQUIRE(replay.collision.restoreLocal(saved));
        CHECK(test::liveDigest(replay.collision) == test::liveDigest(native.collision));
        CHECK(replay.collision.triangleCount() == 3);
        CHECK(replay.device.texturesCreated == uploads);
        const Vec3 centre{transform[3]};
        const bool collapsedNow = step == 2;
        const Vec3 normal =
            collapsedNow ? Vec3{0, 0, 1} : glm::normalize(Mat3{transform} * Vec3{0, 0, 1});
        const auto nativeFloor = native.collision.floorAt(centre, 1, 1);
        const auto replayFloor = replay.collision.floorAt(centre, 1, 1);
        REQUIRE(replayFloor.has_value() == nativeFloor.has_value());
        CHECK(nativeFloor.has_value() == !collapsedNow);
        if (nativeFloor) {
            CHECK(nativeFloor->object == 8);
            CHECK(nativeFloor->objectFlags == 4);
            CHECK(glm::abs(nativeFloor->y - centre.y) < 0.00001f);
            CHECK(replayFloor->object == nativeFloor->object);
            CHECK(replayFloor->objectFlags == nativeFloor->objectFlags);
            CHECK(replayFloor->normal == nativeFloor->normal);
            CHECK(replayFloor->y == nativeFloor->y);
        }
        const Vec3 from = centre + normal;
        const Vec3 to = centre - normal;
        const auto nativeRay = native.collision.pickSurface(from, to);
        CHECK(nativeRay.has_value() == !collapsedNow);
        CHECK(replay.collision.pickSurface(from, to) == nativeRay);
        CHECK(native.collision.wallBetween(from, to) == !collapsedNow);
        CHECK(replay.collision.wallBetween(from, to) == !collapsedNow);
        CHECK(native.collision.wallBetween(from, to, 0.25f) == !collapsedNow);
        CHECK(replay.collision.wallBetween(from, to, 0.25f) == !collapsedNow);
        const Vec3 body = centre + normal * 0.1f;
        const auto nativeContacts =
            native.collision.surfaceContacts(body, 0.5f, centre.y - 0.5f, centre.y + 0.5f);
        const auto replayContacts =
            replay.collision.surfaceContacts(body, 0.5f, centre.y - 0.5f, centre.y + 0.5f);
        CHECK(std::ranges::any_of(nativeContacts, [](const auto& contact) {
                  return contact.object == 1000;
              }) == !collapsedNow);
        REQUIRE(replayContacts.size() == nativeContacts.size());
        for (usize contact = 0; contact < nativeContacts.size(); ++contact) {
            CHECK(replayContacts[contact].object == nativeContacts[contact].object);
            CHECK(replayContacts[contact].point == nativeContacts[contact].point);
        }
        const auto nativeResolved =
            native.collision.resolveWalls(body, 0.5f, centre.y - 0.5f, centre.y + 0.5f);
        CHECK((nativeResolved != body) == !collapsedNow);
        CHECK(replay.collision.resolveWalls(body, 0.5f, centre.y - 0.5f, centre.y + 0.5f) ==
              nativeResolved);
        CHECK(replay.collision.sweepWalls(from, to, 0.5f, centre.y - 0.5f, centre.y + 0.5f) ==
              native.collision.sweepWalls(from, to, 0.5f, centre.y - 0.5f, centre.y + 0.5f));
    }
    // The static appended floor and the original local triangles survive every
    // rewind, including the zero-scale interval and subsequent expansion.
    const auto floor = replay.collision.floorAt({50, -3, 0}, 1, 1);
    REQUIRE(floor);
    CHECK(floor->object == 2000);
    CHECK(floor->objectFlags == 4);
    WorldCollision::LocalSnapshot finished;
    replay.collision.saveLocal(finished);
    CHECK(replay.collision.equalLocal(finished, initial));
}
} // namespace
