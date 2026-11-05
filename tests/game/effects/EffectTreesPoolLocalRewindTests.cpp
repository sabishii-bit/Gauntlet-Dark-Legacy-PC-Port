#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/NativeStateDigest.h"

#include "NativeStateSupport.h"
#include "game/level/NativeStateResources.h"
#include "game/party/ProjectileCheckpointFixture.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** A pool's native digest, its archive named alike for every pool that borrows it. */
NativeStateDigest::Digest digestOf(const EffectTrees& pool, const ItemArchive& archive) {
    NativeStateResources resources;
    REQUIRE(resources.registerArchive(archive, "stage/effects"));
    return test::liveDigest(pool, resources);
}

void compareDraws(EffectTrees& native, EffectTrees& replay, test::FakeRenderDevice& device) {
    const CameraFrame camera;
    for (const f32 blend : {0.0f, 0.5f, 1.0f}) {
        device.draws.clear();
        native.draw(device, Mat4{1}, {}, &camera, blend);
        const auto expected = device.draws;
        device.draws.clear();
        replay.draw(device, Mat4{1}, {}, &camera, blend);
        REQUIRE(device.draws.size() == expected.size());
        for (usize index = 0; index < expected.size(); ++index) {
            CHECK(device.draws[index].vertices == expected[index].vertices);
            CHECK(device.draws[index].texture == expected[index].texture);
            CHECK(device.draws[index].state == expected[index].state);
        }
    }
}

TEST_CASE("full effect pool rewinds native identities tails shared clocks and draw interpolation",
          "[effect-local-rewind]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    test::ProjectileCheckpointFixture fixture;
    EffectTrees native;
    EffectTrees replay;
    EffectTrees::Setting setting;
    setting.persistent = true;
    u32 first = 0;
    u32 second = 0;
    for (auto* pool : {&native, &replay}) {
        first = pool->startSet(fixture.device, fixture.archive, "FLIGHT", {}, setting);
        REQUIRE(first != 0);
        REQUIRE(pool->tag(first, {2, 49, 2, 0}));
        pool->update(0.117f);
        second = pool->startSet(fixture.device, fixture.archive, "FLIGHT", {4, 5, 6}, setting);
        REQUIRE(second != 0);
        pool->capturePresentation();
        pool->placeAt(first, glm::translate(Mat4{1}, Vec3{1, 2, 3}), Vec3{0, 1, 2});
        pool->update(seconds);
        pool->finish(first);
    }
    REQUIRE(native.count() == 2);
    const auto retiring = native.checkpoint(first);
    REQUIRE(retiring);
    REQUIRE(retiring->retiring);
    REQUIRE(retiring->particles.field.entries.size() == 1);
    REQUIRE(!retiring->particles.field.entries[0].emitter.particles.empty());
    const auto uploads = fixture.device.texturesCreated;
    // The replay leaves on a wrong branch, then rewinds to where the native pool stands.
    EffectTrees::LocalSnapshot saved;
    replay.saveLocal(saved);
    replay.moveTo(second, {-9, 8, 7});
    replay.update(0.3f);
    REQUIRE(digestOf(replay, fixture.archive) != digestOf(native, fixture.archive));
    REQUIRE(replay.restoreLocal(saved));
    CHECK(digestOf(replay, fixture.archive) == digestOf(native, fixture.archive));
    compareDraws(native, replay, fixture.device);
    for (s32 tick = 0; tick < 90; ++tick) {
        CAPTURE(tick, seconds);
        native.capturePresentation();
        replay.capturePresentation();
        const auto position = Vec3{0.1f * static_cast<f32>(tick), 2, 3};
        native.moveTo(second, position);
        replay.moveTo(second, position);
        native.update(seconds);
        replay.update(seconds);
        CHECK(digestOf(native, fixture.archive) == digestOf(replay, fixture.archive));
        compareDraws(native, replay, fixture.device);
    }
    CHECK_FALSE(native.playing(first));
    native.stop(second);
    replay.stop(second);
    CHECK(native.count() == 0);
    // Retiring all effects must not reset the clock or recycle identities.
    const auto later = native.startSet(fixture.device, fixture.archive, "FLIGHT", {}, setting);
    const auto replayedLater =
        replay.startSet(fixture.device, fixture.archive, "FLIGHT", {}, setting);
    CHECK(later == replayedLater);
    CHECK(digestOf(native, fixture.archive) == digestOf(replay, fixture.archive));
    CHECK(fixture.device.texturesCreated == uploads);
}

TEST_CASE("effect pool rewind preserves sparse identities and complete native particles",
          "[effect-local-rewind]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    test::ProjectileCheckpointFixture fixture;
    EffectTrees native;
    std::array<u32, 48> ids{};
    for (usize index = 0; index < ids.size(); ++index) {
        EffectTrees::Setting setting;
        setting.persistent = true;
        setting.scale = 0.5f + static_cast<f32>(index) / 32;
        setting.velocity = {static_cast<f32>(index) / 8, 1, -2};
        setting.alpha = 0.25f + static_cast<f32>(index) / 96;
        ids[index] = native.startSet(fixture.device, fixture.archive, "FLIGHT",
                                     {static_cast<f32>(index), 2, 3}, setting);
        REQUIRE(ids[index] != 0);
        REQUIRE(native.tag(ids[index], {2, 71, static_cast<u16>(index), 0}));
    }
    for (usize index = 0; index < ids.size(); index += 7) {
        native.stop(ids[index]);
        CHECK_FALSE(native.checkpoint(ids[index]));
    }
    native.capturePresentation();
    native.update(0.117f);
    for (usize index = 1; index < ids.size(); index += 5) {
        native.finish(ids[index]);
    }
    const auto textures = fixture.device.texturesCreated;
    const auto uploads = fixture.device.textureUpdates;
    REQUIRE(native.count() > 32);
    usize particles = 0;
    for (usize index = 0; index < native.count(); ++index) {
        particles += native.effect(index).particles.field().particleCount();
    }
    REQUIRE(particles > 0);
    EffectTrees::LocalSnapshot saved;
    native.saveLocal(saved);
    std::vector<NativeStateDigest::Digest> expected;
    for (s32 tick = 0; tick < 8; ++tick) {
        native.capturePresentation();
        native.update(seconds);
        expected.push_back(digestOf(native, fixture.archive));
    }
    REQUIRE(native.restoreLocal(saved));
    for (s32 tick = 0; tick < 8; ++tick) {
        CAPTURE(tick, seconds);
        native.capturePresentation();
        native.update(seconds);
        CHECK(digestOf(native, fixture.archive) == expected[static_cast<usize>(tick)]);
    }
    CHECK(fixture.device.texturesCreated == textures);
    CHECK(fixture.device.textureUpdates == uploads);
}

TEST_CASE("standalone emitter pool resumes exact RNG and attached trail topology",
          "[effect-local-rewind]") {
    test::ProjectileCheckpointFixture fixture;
    ParticleDescriptor descriptor;
    descriptor.texture = "WHITE";
    descriptor.emitFrames = ParticleDescriptor::kEndless;
    descriptor.rate = {0.7f, 0.7f, 0.7f, 0.7f};
    descriptor.rateRandom = 0.4f;
    descriptor.particleLife = 18;
    descriptor.volume = {1, 2, 3};
    descriptor.speed = 0.3f;
    EffectTrees native;
    EffectTrees replay;
    u32 id = 0;
    for (auto* pool : {&native, &replay}) {
        id = pool->startParticles(fixture.device, fixture.archive, descriptor, 0, Mat4{1});
        REQUIRE(id != 0);
        pool->update(0.117f);
    }
    // The replay draws ahead on a wrong branch, then rewinds its random state with the rest.
    EffectTrees::LocalSnapshot saved;
    replay.saveLocal(saved);
    replay.update(0.3f);
    REQUIRE(digestOf(replay, fixture.archive) != digestOf(native, fixture.archive));
    REQUIRE(replay.restoreLocal(saved));
    for (s32 tick = 0; tick < 80; ++tick) {
        if (tick == 10) {
            native.finish(id);
            replay.finish(id);
        }
        native.capturePresentation();
        replay.capturePresentation();
        native.update(1.0f / 60);
        replay.update(1.0f / 60);
        CHECK(digestOf(native, fixture.archive) == digestOf(replay, fixture.archive));
        compareDraws(native, replay, fixture.device);
    }
    CHECK_FALSE(native.playing(id));
}

/** A morphing archive whose artwork the prototypes bind before any pool counts uploads. */
struct MorphFixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    TextureAnimator textures;
    EffectTrees prototypes;

    MorphFixture() {
        const auto directory = test::projectileCheckpointArchive();
        writeTextFile(directory / "animations.json", R"({
          "particles":[{"enables":16928,"texture":"WHITE","particleLife":[1,0.2],
            "rate":[30,30,30,30],"flags":128,"flagMask":128}],
          "trees":[
            {"name":"ENTRANCE","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
              {"name":"EMITTER","parent":0,"position":[0,1,0],"particle":0}],
             "sequences":[{"name":"ACTIVE","frames":4,"frameRate":30,
               "tracks":[{"node":0,"flags":32,"frames":[0,3],"values":[0,2]}]}]},
            {"name":"LOOP","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
              {"name":"JOINT","parent":0,"position":[1,2,0]},
              {"name":"EMITTER","parent":1,"position":[0,1,0],"particle":0}],
             "sequences":[{"name":"ACTIVE","frames":9,"frameRate":30,
               "tracks":[{"node":1,"flags":16,"frames":[0,8],"values":[1,3]}]}]}]})");
        REQUIRE(archive.load(directory));
        textures.bind(archive.trees.textureAnimations(), archive.textures, device);
        EffectTrees::Setting setting;
        setting.persistent = true;
        REQUIRE(prototypes.startSet(device, archive, "ENTRANCE", {}, setting) != 0);
        REQUIRE(prototypes.startSet(device, archive, "LOOP", {}, setting) != 0);
    }
};

void compareTexture(const Texture* actual, const Texture* expected) {
    REQUIRE((actual != nullptr) == (expected != nullptr));
    if (actual == nullptr) {
        return;
    }
    const auto* a = dynamic_cast<const test::FakeTexture*>(actual);
    const auto* b = dynamic_cast<const test::FakeTexture*>(expected);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->width() == b->width());
    CHECK(a->height() == b->height());
    CHECK((a->pixels == b->pixels));
}

void compareMorphDraws(EffectTrees& native, EffectTrees& replay, test::FakeRenderDevice& device) {
    const CameraFrame camera;
    for (const f32 blend : {0.0f, 0.5f, 1.0f}) {
        device.draws.clear();
        native.draw(device, Mat4{1}, {}, &camera, blend);
        const auto expected = device.draws;
        device.draws.clear();
        replay.draw(device, Mat4{1}, {}, &camera, blend);
        REQUIRE(device.draws.size() == expected.size());
        for (usize index = 0; index < expected.size(); ++index) {
            const auto& actual = device.draws[index];
            const auto& oracle = expected[index];
            CHECK(actual.vertices == oracle.vertices);
            CHECK(actual.transform == oracle.transform);
            compareTexture(actual.texture, oracle.texture);
            compareTexture(actual.state.lightmap, oracle.state.lightmap);
            compareTexture(actual.state.maskedTexture, oracle.state.maskedTexture);
            compareTexture(actual.state.nextTexture, oracle.state.nextTexture);
            auto normalized = actual.state;
            normalized.lightmap = oracle.state.lightmap;
            normalized.maskedTexture = oracle.state.maskedTexture;
            normalized.nextTexture = oracle.state.nextTexture;
            CHECK(normalized == oracle.state);
        }
    }
}

TEST_CASE("effect pool chains rewind native morph timing particle birth and presentation cuts",
          "[effect-local-rewind][effect-pool-morph]") {
    const f32 seconds = GENERATE(1.0f / 30, 1.0f / 60);
    const bool timedMorph = GENERATE(false, true);
    CAPTURE(seconds, timedMorph);
    MorphFixture host;
    EffectTrees native;
    EffectTrees replay;
    EffectTrees::Setting setting;
    setting.seconds = 3;
    setting.then = "LOOP";
    setting.velocity = {2, 1, 0};
    setting.stretch = {2, 0.5f, 1};
    setting.alpha = 0.75f;
    setting.unlit = true;
    setting.depthWrite = false;
    if (timedMorph) {
        setting.morphIn = 0.137f;
        setting.holdForMorph = true;
    }
    for (auto* pool : {&native, &replay}) {
        const auto retired = pool->startSet(host.device, host.archive, "LOOP", {}, {});
        REQUIRE(retired != 0);
        pool->stop(retired);
        const auto id = pool->startSet(host.device, host.archive, "ENTRANCE", {4, 5, 6}, setting);
        REQUIRE(id > retired);
        REQUIRE(pool->tag(id, {3, 51, 2, 1}));
        pool->capturePresentation();
        pool->update(seconds * 0.5f);
    }
    REQUIRE(native.count() == 1);
    REQUIRE(native.effect(0).then == "LOOP");
    const auto textures = host.device.texturesCreated;
    const auto uploads = host.device.textureUpdates;
    EffectTrees::LocalSnapshot nativeBaseline;
    EffectTrees::LocalSnapshot replayBaseline;
    native.saveLocal(nativeBaseline);
    replay.saveLocal(replayBaseline);
    // The replay plays on past the pending morph on a wrong branch, then rewinds before it.
    for (s32 tick = 0; tick < 12; ++tick) {
        replay.capturePresentation();
        replay.update(seconds * 1.5f);
    }
    REQUIRE(digestOf(replay, host.archive) != digestOf(native, host.archive));
    REQUIRE(replay.restoreLocal(replayBaseline));
    CHECK(digestOf(replay, host.archive) == digestOf(native, host.archive));
    compareMorphDraws(native, replay, host.device);
    bool crossedMorph = false;
    bool emittedAfterMorph = false;
    for (s32 tick = 0; tick < 35; ++tick) {
        CAPTURE(tick);
        native.capturePresentation();
        replay.capturePresentation();
        native.update(seconds);
        replay.update(seconds);
        CHECK(digestOf(replay, host.archive) == digestOf(native, host.archive));
        compareMorphDraws(native, replay, host.device);
        const auto& effect = native.effect(0);
        if (effect.previousTree != nullptr && effect.previousTree != effect.tree) {
            // The tick that changes topology: its previous pose belongs to the old tree.
            crossedMorph = true;
            REQUIRE(effect.name == "LOOP");
            REQUIRE(effect.previousTree->name == "ENTRANCE");
            REQUIRE(effect.then.empty());
        }
        if (effect.name == "LOOP") {
            emittedAfterMorph |= effect.particles.field().particleCount() > 0;
        }
    }
    REQUIRE(crossedMorph);
    REQUIRE(emittedAfterMorph);
    const auto final = digestOf(native, host.archive);
    // Rewind both pools back before the pending morph.
    REQUIRE(native.restoreLocal(nativeBaseline));
    REQUIRE(replay.restoreLocal(replayBaseline));
    for (s32 tick = 0; tick < 35; ++tick) {
        native.capturePresentation();
        replay.capturePresentation();
        native.update(seconds);
        replay.update(seconds);
    }
    CHECK(digestOf(native, host.archive) == final);
    CHECK(digestOf(replay, host.archive) == final);
    CHECK(host.device.texturesCreated == textures);
    CHECK(host.device.textureUpdates == uploads);
}
} // namespace
