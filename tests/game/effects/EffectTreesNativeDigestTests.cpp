#include <array>
#include <functional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/io/NativeStateDigest.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/effects/EffectTrees.h"
#include "game/level/NativeStateResources.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Digest = NativeStateDigest::Digest;

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    TextureSet lenderA, lenderB;
    NativeStateResources resources;
    EffectTrees effects;

    explicit Fixture(std::string_view name) {
        const auto root = test::scratchDirectory(name);
        writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "textures.json", R"({"bitmaps":[
          {"name":"WHITE","file":"white.png","width":2,"height":2},
          {"name":"WHITE1","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({
          "textureAnimations":[{"name":"WHITE","texture":0,"source":0,
            "frames":2,"rate":1}],
          "particles":[{"enables":16928,"texture":"WHITE","particleLife":[1,0],
            "rate":[30,30,30,30],"flags":128,"flagMask":128}],
          "trees":[
            {"name":"FLIGHT","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
              {"name":"EMITTER","parent":0,"position":[0,1,0],"particle":0}],
             "sequences":[{"name":"ACTIVE","frames":9,"frameRate":30,
               "tracks":[{"node":0,"flags":32,"frames":[0,8],"values":[0,2]}]}]},
            {"name":"NEXT","nodes":[
              {"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]},
              {"name":"JOINT","parent":0,"position":[1,2,3]},
              {"name":"EMITTER","parent":1,"position":[0,1,0],"particle":0}],
             "sequences":[{"name":"ACTIVE","frames":5,"frameRate":30,
               "tracks":[{"node":1,"flags":16,"frames":[0,4],"values":[1,3]}]}]}]})");
        test::convertModelFixture(root);
        REQUIRE(archive.load(root));
        REQUIRE(lenderA.load(root));
        REQUIRE(lenderB.load(root));
        REQUIRE(resources.registerArchive(archive, "effects/source"));
        REQUIRE(resources.registerTextures(lenderA, "effects/lender-a"));
        REQUIRE(resources.registerTextures(lenderB, "effects/lender-b"));
        REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
    }

    u32 start(bool morph = false, bool wrong = false) {
        EffectTrees::Setting setting;
        setting.persistent = true;
        setting.light = EffectTrees::Light{{0.2f, 0.4f, 0.8f}, 4, true};
        setting.velocity = wrong ? Vec3{-9, 7, 1} : Vec3{1, 2, 3};
        if (morph) {
            setting.then = "NEXT";
            setting.morphIn = 0.04f;
            setting.holdForMorph = true;
        }
        const auto id = effects.startSet(device, archive, "FLIGHT", {3, 4, 5}, setting);
        REQUIRE(id != 0);
        return id;
    }

    static ParticleDescriptor trail() {
        ParticleDescriptor descriptor;
        descriptor.emitFrames = 6;
        descriptor.fadeFrames = 4;
        descriptor.particleLife = 8;
        descriptor.rate = {1, 1, 1, 1};
        return descriptor;
    }

    Digest hash() {
        REQUIRE(effects.registerNativeResources(resources));
        NativeStateDigest digest(resources.resources());
        REQUIRE(effects.appendNativeDigest(digest));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    }

    Digest savedHash(const EffectTrees::LocalSnapshot& saved) {
        REQUIRE(effects.registerNativeResources(resources));
        NativeStateDigest digest(resources.resources());
        REQUIRE(effects.appendLocalDigest(digest, saved));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    }

    void step(u32 tick, bool wrong = false) {
        effects.capturePresentation();
        if (tick % 3 == 0) {
            start(true, wrong);
        }
        if (tick % 4 == 0) {
            REQUIRE(effects.startParticles(device, archive, trail(), 0,
                                           glm::translate(Mat4{1}, Vec3{6, 5, 4})) != 0);
        }
        if ((effects.count() != 0u) && tick % 5 == 0) {
            effects.placeAt(effects.effect(0).id,
                            glm::translate(Mat4{1}, wrong ? Vec3{-8} : Vec3{1, 2, 3}),
                            Vec3{1, 0, 2});
        }
        if ((effects.count() != 0u) && tick % 7 == 0) {
            effects.finish(effects.effect(0).id);
        }
        effects.update(1.0f / 60);
    }
};

TEST_CASE("effect native hashes agree across independent resources and draw cadences",
          "[native-digest][effect-native-digest]") {
    Fixture a("effect-digest-owner-a");
    Fixture b("effect-digest-owner-b");
    a.start(true);
    b.start(true);
    REQUIRE(a.effects.effect(0).tree != b.effects.effect(0).tree);
    REQUIRE(a.effects.effect(0).model != b.effects.effect(0).model);
    CHECK(a.hash() == b.hash());
    const CameraFrame camera;
    for (u32 tick = 1; tick <= 24; ++tick) {
        CAPTURE(tick);
        a.step(tick);
        b.step(tick);
        const auto before = b.hash();
        for (const f32 alpha : {-1.0f, 0.0f, 0.37f, 1.0f}) {
            b.effects.draw(b.device, Mat4{1}, {}, &camera, alpha);
            CHECK(b.hash() == before);
        }
        CHECK(a.hash() == b.hash());
        b.device.draws.clear();
    }
    const auto created = b.device.texturesCreated;
    const auto updates = b.device.textureUpdates;
    const auto draws = b.device.draws.size();
    CHECK(b.hash() == a.hash());
    CHECK(b.device.texturesCreated == created);
    CHECK(b.device.textureUpdates == updates);
    CHECK(b.device.draws.size() == draws);
}

TEST_CASE("effect native digest covers each retained value family and recursive prototypes",
          "[native-digest][effect-native-digest]") {
    Fixture f("effect-digest-hidden-values");
    const auto id = f.start(true);
    REQUIRE(f.effects.tag(id, {1, 20, 2, 3}));
    f.effects.attachTrail(id, Fixture::trail(), f.archive.textures.texture(f.device, 0));
    f.effects.capturePresentation();
    f.effects.update(1.0f / 30);
    EffectTrees::LocalSnapshot saved;
    f.effects.saveLocal(saved);
    const auto baseline = f.hash();
    CHECK(f.savedHash(saved) == baseline);
    using Mutate = void (*)(EffectTrees::EffectState&);
    const std::array<Mutate, 45> changes{{
        [](auto& e) { ++e.instance; },
        [](auto& e) { ++e.continuity; },
        [](auto& e) { e.name += "X"; },
        [](auto& e) { ++e.id; },
        [](auto& e) { e.position.x += 1; },
        [](auto& e) { e.scale += 1; },
        [](auto& e) { e.stretch.z += 1; },
        [](auto& e) { e.fadeSeconds += 1; },
        [](auto& e) { e.yaw += 1; },
        [](auto& e) { e.velocity.y += 1; },
        [](auto& e) { e.repeats = !e.repeats; },
        [](auto& e) { e.timed = !e.timed; },
        [](auto& e) { e.persistent = !e.persistent; },
        [](auto& e) { e.emitParticles = !e.emitParticles; },
        [](auto& e) { e.missingObjectsAreEmpty = !e.missingObjectsAreEmpty; },
        [](auto& e) { e.additive = !e.additive; },
        [](auto& e) { e.then += "X"; },
        [](auto& e) { *e.morphIn += 1; },
        [](auto& e) { e.holdForMorph = !e.holdForMorph; },
        [](auto& e) { e.shrinks = !e.shrinks; },
        [](auto& e) { e.retiring = !e.retiring; },
        [](auto& e) { e.device = nullptr; },
        [](auto& e) { e.model.reset(); },
        [](auto& e) { e.pose.rest(*e.tree); },
        [](auto& e) { e.player.setSpeed(1.5f); },
        [](auto& e) { e.secondsLeft += 1; },
        [](auto& e) { e.unlit = !e.unlit; },
        [](auto& e) { e.depthWrite = !e.depthWrite; },
        [](auto& e) { e.tint.r = 17; },
        [](auto& e) { e.alpha *= 0.5f; },
        [](auto& e) { e.playbackRate += 1; },
        [](auto& e) { e.light->radius += 1; },
        [](auto& e) { e.lived += 1; },
        [](auto& e) { e.trails.setEmitting(0, false); },
        [](auto& e) { e.particles.stop(); },
        [](auto& e) { e.attachment = Mat4{2}; },
        [](auto& e) { e.flightDirection = Vec3{1, 0, 0}; },
        [](auto& e) { e.particleTextureSlot = 0; },
        [](auto& e) { e.previousTransform[3].y += 1; },
        [](auto& e) { e.previousDirection = Vec3{1}; },
        [](auto& e) { e.previousTree = nullptr; },
        [](auto& e) { ++e.previousGeneration; },
        [](auto& e) { e.previousFrame += 1; },
        [](auto& e) { e.presentationCaptured = !e.presentationCaptured; },
        [](auto& e) { ++e.provenance->occurrence; },
    }};
    for (usize index = 0; index < changes.size(); ++index) {
        CAPTURE(index);
        REQUIRE(f.effects.restoreLocal(saved));
        // The accessor is read-only gameplay API; this test owns the non-const
        // pool and alters its actual retained record, not a parallel test DTO.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        changes[index](const_cast<EffectTrees::Effect&>(f.effects.effect(0)));
        CHECK(f.hash() != baseline);
        CHECK(f.savedHash(saved) == baseline);
    }
    REQUIRE(f.effects.restoreLocal(saved));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto& current = const_cast<EffectTrees::Effect&>(f.effects.effect(0));
    auto prototype = std::make_shared<EffectTrees::Effect>(current);
    current.morphPrototype = prototype;
    const auto nested = f.hash();
    CHECK(nested != baseline);
    prototype->velocity.x += 1;
    CHECK(f.hash() != nested);
    prototype->morphPrototype = prototype;
    NativeStateDigest cycle(f.resources.resources());
    CHECK_FALSE(f.effects.appendNativeDigest(cycle));
    CHECK_FALSE(cycle.checkedFinish());
    prototype->morphPrototype.reset();
    REQUIRE(f.effects.restoreLocal(saved));
}

TEST_CASE("effect native snapshots hash captured clocks through one to eight tick replay",
          "[native-digest][effect-native-digest][local-rewind]") {
    Fixture f("effect-digest-native-replay");
    f.start();
    f.step(1);
    EffectTrees::LocalSnapshot saved;
    f.effects.saveLocal(saved);
    const auto baseline = f.hash();
    for (u32 ticks = 1; ticks <= 8; ++ticks) {
        CAPTURE(ticks);
        REQUIRE(f.effects.restoreLocal(saved));
        std::array<Digest, 8> expected{};
        for (u32 tick = 0; tick < ticks; ++tick) {
            f.step(tick);
            expected[tick] = f.hash();
        }
        CHECK(f.savedHash(saved) == baseline);
        REQUIRE(f.effects.restoreLocal(saved));
        for (u32 tick = 0; tick < ticks; ++tick) {
            f.step(tick, true);
        }
        CHECK(f.hash() != expected[ticks - 1]);
        CHECK(f.savedHash(saved) == baseline);
        REQUIRE(f.effects.restoreLocal(saved));
        CHECK(f.hash() == baseline);
        for (u32 tick = 0; tick < ticks; ++tick) {
            f.step(tick);
            CHECK(f.hash() == expected[tick]);
        }
    }
}

TEST_CASE("effect native digest distinguishes bank ordering clocks and retired identities",
          "[native-digest][effect-native-digest]") {
    Fixture f("effect-digest-pool-values");
    const auto cold = f.hash();
    f.start();
    f.effects.reset();
    CHECK(f.hash() != cold); // nextInstance survives reset even with no active effects.
    const auto reset = f.hash();
    const std::array<TextureSet*, 2> ab{&f.lenderA, &f.lenderB};
    const std::array<TextureSet*, 2> ba{&f.lenderB, &f.lenderA};
    f.effects.setTextureLenders(ab);
    const auto forward = f.hash();
    CHECK(forward != reset);
    f.effects.setTextureLenders(ba);
    CHECK(f.hash() != forward);
    f.start();
    const auto active = f.hash();
    f.effects.update(0.25f);
    CHECK(f.hash() != active);
    const auto clock = f.hash();
    f.effects.capturePresentation();
    CHECK(f.hash() != clock);
}

TEST_CASE("effect native digest rejects unknown resources and foreign stale history",
          "[native-digest][effect-native-digest]") {
    Fixture f("effect-digest-guards");
    f.start();
    EffectTrees::LocalSnapshot saved;
    const EffectTrees::LocalSnapshot empty{};
    f.effects.saveLocal(saved);
    const auto baseline = f.hash();
    NativeStateDigest unresolved;
    CHECK_FALSE(f.effects.appendNativeDigest(unresolved));
    CHECK_FALSE(unresolved.checkedFinish());
    const EffectTrees other;
    NativeStateDigest foreign(f.resources.resources());
    CHECK_FALSE(other.appendLocalDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());
    NativeStateDigest missing(f.resources.resources());
    CHECK_FALSE(f.effects.appendLocalDigest(missing, empty));
    CHECK_FALSE(missing.checkedFinish());
    const test::FakeTexture unregistered(2, 2);
    f.effects.attachTrail(f.effects.effect(0).id, Fixture::trail(), unregistered);
    NativeStateDigest unknown(f.resources.resources());
    CHECK_FALSE(f.effects.appendNativeDigest(unknown));
    CHECK_FALSE(unknown.checkedFinish());
    REQUIRE(f.effects.restoreLocal(saved));
    CHECK(f.hash() == baseline);
    f.effects.clear();
    NativeStateDigest stale(f.resources.resources());
    CHECK_FALSE(f.effects.appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());
}

TEST_CASE("effect texture bindings hash authored and resolved values separately from clocks",
          "[native-digest][effect-native-digest][texture-binding-digest]") {
    Fixture a("effect-bank-digest-a");
    Fixture b("effect-bank-digest-b");
    TextureAnimator left;
    TextureAnimator right;
    auto infos = a.archive.trees.textureAnimations();
    left.bind(infos, a.archive.textures, a.device);
    right.bind(infos, b.archive.textures, b.device);
    REQUIRE(a.resources.refreshArchive(a.archive));
    REQUIRE(b.resources.refreshArchive(b.archive));
    const auto binding = [](const TextureAnimator& animator, NativeStateResources& resources) {
        NativeStateDigest digest(resources.resources());
        REQUIRE(animator.appendBindingDigest(digest));
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    };
    const auto baseline = binding(left, a.resources);
    CHECK(binding(right, b.resources) == baseline);
    left.advance(0.17f);
    CHECK(binding(left, a.resources) == baseline);
    for (s32 field = 0; field < 9; ++field) {
        CAPTURE(field);
        auto changed = infos;
        switch (field) {
        case 0: changed[0].name += "X"; break;
        case 1: changed[0].frameName += "X"; break;
        case 2: ++changed[0].texture; break;
        case 3: ++changed[0].source; break;
        case 4: --changed[0].frames; break;
        case 5: ++changed[0].start; break;
        case 6: ++changed[0].rate; break;
        case 7: ++changed[0].offset; break;
        case 8: changed[0].flag = 0; break;
        default: break;
        }
        right.bind(changed, b.archive.textures, b.device);
        REQUIRE(b.resources.refreshArchive(b.archive));
        CHECK(binding(right, b.resources) != baseline);
    }
    NativeStateDigest unknown;
    CHECK_FALSE(left.appendBindingDigest(unknown));
    CHECK_FALSE(unknown.checkedFinish());
}

TEST_CASE("effect native saved digest restores a prepared negative scroll bank exactly",
          "[native-digest][effect-native-digest][texture-signed-start]") {
    Fixture f("effect-digest-signed-clock");
    TextureAnimationInfo info;
    info.name = "WHITE";
    info.texture = 0;
    info.source = TextureAnimationInfo::kScrollU;
    info.frames = 4;
    info.start = -1;
    info.rate = 1;
    TextureAnimator textures;
    const std::array animations{info};
    textures.bind(animations, f.archive.textures, f.device);
    EffectTrees::Prepared prepared;
    prepared.archive = &f.archive;
    prepared.textures = &textures;
    REQUIRE(f.effects.prepareBank(prepared));
    EffectTrees::LocalSnapshot saved;
    f.effects.saveLocal(saved);
    const auto initial = f.hash();
    f.effects.update(1.0f / 30);
    const auto* bank = f.effects.textureAnimator(&f.archive, {});
    REQUIRE(bank);
    CHECK(bank->counter(0) == 0);
    CHECK(f.savedHash(saved) == initial);
    REQUIRE(f.effects.restoreLocal(saved));
    CHECK(bank->counter(0) == -1);
    CHECK(bank->motion(0).offset.x == -0.25f);
    CHECK(f.hash() == initial);
}

TEST_CASE("effect native digest bytes stay pinned for a tagged morphing effect",
          "[native-digest][effect-native-digest][native-digest-pin]") {
    Fixture f("effect-digest-pin");
    const auto id = f.start(true);
    REQUIRE(f.effects.tag(id, {1, 20, 2, 3}));
    // SHA-256 of the native digest of the effect pool above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kDigest =
        "87eed365b0c7db18729cf5e90016a36004c680d988fa77ab1ac70cffa9291c65";
    CHECK(test::hexBytes(f.hash()) == kDigest);
}
} // namespace
