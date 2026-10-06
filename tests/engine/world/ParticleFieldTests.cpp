#include <array>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/ParticleField.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "SampleLevel.h"
#include "TestSupport.h"

namespace {

using namespace gdl;

TEST_CASE("animated world markers move new emissions without carrying existing particles",
          "[world][particles][particle-attachment]") {
    const auto dir = test::sampleLevel("particle-field-attachment");
    writeTextFile(dir / "world.json", R"({
      "objects":[
        {"name":"GROUP","position":[10,0,0],"flags":4096,"child":1,"next":4},
        {"name":"PSYSA","position":[1,0,0],"flags":6144,"next":2},
        {"name":"PSYSB","position":[0,2,0],"flags":6144,"next":3},
        {"name":"PSYSA_STATIC","position":[0,0,3],"flags":2048},
        {"name":"WALL","position":[0,0,0]}],
      "particles":[
        {"id":"A","enables":25248,"particleLife":[3,0],"rate":[30,30,30,30],
         "direction":[1,0,0],"speed":30,"texture":"STONE"},
        {"id":"B","enables":25248,"particleLife":[3,0],"rate":[30,30,30,30],
         "direction":[1,0,0],"speed":30,"texture":"GLASS"}]})");
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    REQUIRE(models.load(dir));
    REQUIRE(textures.load(dir));
    REQUIRE(layout.load(dir));
    WorldScene scene;
    REQUIRE(scene.build(layout, models, textures, device));
    ParticleField field;
    field.bind(layout, textures, device);
    REQUIRE(field.size() == 3);
    // Different textures reorder entries for batching; attachments belong to the
    // emitter, not to its index in the source layout or sorted field.
    const auto emitterAt = [&](usize object) {
        for (usize i = 0; i < field.size(); ++i) {
            if (Vec3{field.emitter(i).node()[3]} == layout.worldPosition(object)) {
                return i;
            }
        }
        return field.size();
    };
    const usize first = emitterAt(1);
    const usize second = emitterAt(2);
    const usize still = emitterAt(3);
    REQUIRE(first < field.size());
    REQUIRE(second < field.size());
    REQUIRE(still < field.size());
    REQUIRE(field.textureOf(first) != field.textureOf(second));
    const ParticleDescriptor independent = field.emitter(first).descriptor();
    const Mat4 independentNode = glm::translate(Mat4{1}, Vec3{-9, 8, 7});
    const usize own = field.start(independent, independentNode, &device.whiteTexture());
    field.syncNodes(scene);
    field.step(1.0f / 30);
    REQUIRE(field.particleCount() == 4);
    const Particle old = field.emitter(first).particles()[0];
    CHECK(old.origin == layout.worldPosition(1));
    CHECK(old.velocity == Vec3{1, 0, 0});
    scene.capturePresentation();
    const Mat4 moved =
        glm::rotate(glm::translate(Mat4{1}, Vec3{20, -15, 5}), kHalfPi, Vec3{0, 0, 1});
    scene.setObjectTransform(0, moved);
    field.syncNodes(scene);
    for (const auto& [object, emitter] :
         std::array{std::pair{usize{1}, first}, std::pair{usize{2}, second}}) {
        CHECK(glm::distance(Vec3{field.emitter(emitter).node()[3]},
                            Vec3{scene.worldTransform(object)[3]}) < 0.0001f);
    }
    // A marker without native bit 0x1000 keeps its captured origin, even beneath
    // a moving ancestor. Independently started effects remain caller-controlled.
    CHECK(Vec3{field.emitter(still).node()[3]} == layout.worldPosition(3));
    CHECK(field.emitter(own).node() == independentNode);
    CHECK(field.emitter(first).particles()[0].origin == old.origin);
    CHECK(field.emitter(first).particles()[0].velocity == old.velocity);
    field.step(1.0f / 30);
    REQUIRE(field.particleCount() == 8);
    CHECK(field.emitter(first).positionOf(field.emitter(first).particles()[0]) ==
          old.origin + old.velocity);
    for (const auto& [object, emitter] :
         std::array{std::pair{usize{1}, first}, std::pair{usize{2}, second}}) {
        const auto& born = field.emitter(emitter).particles()[1];
        CHECK(glm::distance(born.origin, Vec3{scene.worldTransform(object)[3]}) < 0.0001f);
        CHECK(glm::distance(born.velocity, Vec3{0, 1, 0}) < 0.0001f);
    }
    CHECK(field.emitter(still).particles()[1].origin == layout.worldPosition(3));
    CHECK(field.emitter(own).particles()[1].origin == Vec3{independentNode[3]});
    // Presentation samples age, never the animated marker's interpolated pose.
    for (const f32 alpha : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        field.draw(device, Mat4{1}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, alpha);
        CHECK(field.particleCount() == 8);
        CHECK(field.emitter(first).particles()[0].origin == old.origin);
        CHECK(field.emitter(first).particles()[0].age == 1);
        CHECK(field.emitter(first).particles()[1].age == 0);
        CHECK(glm::distance(Vec3{field.emitter(first).node()[3]},
                            Vec3{scene.worldTransform(1)[3]}) < 0.0001f);
    }
}

TEST_CASE("particle flipbook pairs are render-only and preserve hard depth cutouts",
          "[world][particles][presentation][texture-blend]") {
    test::FakeRenderDevice device;
    const test::FakeTexture current{1, 1};
    const test::FakeTexture next{1, 1};
    ParticleField field;
    for (usize i = 0; i < 3; ++i) {
        ParticleDescriptor descriptor;
        descriptor.oneShot = true;
        descriptor.maxParticles = 1;
        descriptor.particleLife = 30;
        descriptor.width = {1, 1, 1, 1};
        descriptor.additive = i == 0;
        descriptor.depthWrite = i != 1;
        field.start(descriptor, Mat4{1}, &device.whiteTexture());
    }
    field.step(1.0f / 30);
    REQUIRE(field.particleCount() == 3);
    field.step(1.0f / 60);
    for (usize i = 0; i < field.size(); ++i) {
        field.setTextureBlend(i, current, &next, 0.25f);
        CHECK(field.textureOf(i) == &device.whiteTexture());
    }
    const auto draw = [&](f32 alpha) {
        device.draws.clear();
        field.draw(device, Mat4{1}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, alpha);
        REQUIRE(device.draws.size() == 3);
    };
    draw(0.5f);
    for (usize i = 0; i < 2; ++i) {
        CHECK(device.draws[i].texture == &current);
        CHECK(device.draws[i].state.nextTexture == &next);
        CHECK(device.draws[i].state.effectiveTextureBlend() == 0.25f);
    }
    CHECK(device.draws[2].texture == &device.whiteTexture());
    CHECK(device.draws[2].state.nextTexture == nullptr);
    draw(-1.0f); // paused/native drawing ignores even a retained render override
    for (const auto& call : device.draws) {
        CHECK(call.texture == &device.whiteTexture());
        CHECK(call.state.nextTexture == nullptr);
    }
    for (usize i = 0; i < field.size(); ++i) {
        CHECK(field.emitter(i).particles()[0].age == 0);
    }
    field.clearTextureBlends();
    draw(0.5f);
    CHECK(device.draws[0].texture == &device.whiteTexture());
    field.setTextureBlend(0, current, &next, 0.75f);
    field.setTexture(0, next); // a new native binding invalidates the old pair
    draw(0.5f);
    CHECK(device.draws[0].texture == &next);
    CHECK(device.draws[0].state.nextTexture == nullptr);
    field.setTextureBlend(0, current, &next, 0.75f);
    field.step(1.0f / 60); // even a fractional simulation step expires old draw overrides
    draw(0.5f);
    CHECK(device.draws[0].texture == &next);
    CHECK(device.draws[0].state.nextTexture == nullptr);
    CHECK(field.particleCount() == 3);
    field.setTextureBlend(999, current, &next, 1.0f); // invalid emitter is harmless
}

TEST_CASE("particles batch only matching flipbook pairs and blend fractions",
          "[world][particles][presentation][texture-blend]") {
    test::FakeRenderDevice device;
    const test::FakeTexture current{1, 1};
    const test::FakeTexture next{1, 1};
    ParticleField field;
    ParticleDescriptor descriptor;
    descriptor.oneShot = true;
    descriptor.maxParticles = 1;
    descriptor.particleLife = 30;
    descriptor.additive = true;
    for (usize i = 0; i < 4; ++i) {
        field.start(descriptor, Mat4{1}, &device.whiteTexture());
    }
    field.step(1.0f / 30);
    field.step(1.0f / 60);
    for (usize i = 0; i < field.size(); ++i) {
        field.setTextureBlend(i, current, &next, i < 2 ? 0.25f : 0.75f);
    }
    field.draw(device, Mat4{1}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, 0.5f);
    REQUIRE(device.draws.size() == 2);
    CHECK(device.draws[0].vertices.size() == 12);
    CHECK(device.draws[1].vertices.size() == 12);
    CHECK(device.draws[0].state.textureBlend == 0.25f);
    CHECK(device.draws[1].state.textureBlend == 0.75f);
}

TEST_CASE("particle presentation fills fractional ticks without advancing emission",
          "[world][particles][presentation]") {
    test::FakeRenderDevice device;
    ParticleField field;
    ParticleDescriptor descriptor;
    descriptor.oneShot = true;
    descriptor.maxParticles = 1;
    descriptor.particleLife = 10;
    descriptor.direction = Vec3{1, 0, 0};
    descriptor.speed = 1;
    descriptor.width = {1, 1, 1, 1};
    field.start(descriptor, Mat4{1}, &device.whiteTexture());
    field.step(1.0f / 30);
    field.step(1.0f / 60);
    const auto center = [&](f32 alpha) {
        device.draws.clear();
        field.draw(device, Mat4{1}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, alpha);
        REQUIRE(device.draws.size() == 1);
        const auto& vertices = device.draws.front().vertices;
        return (vertices[0].position.x + vertices[1].position.x) * 0.5f;
    };
    CHECK(center(-1) == Catch::Approx(0));
    for (const f32 alpha : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        CHECK(center(alpha) == Catch::Approx(alpha * 0.5f));
        REQUIRE(field.particleCount() == 1);
        CHECK(field.emitter(0).particles()[0].age == 0);
    }
    field.step(1.0f / 60);
    CHECK(center(0) == Catch::Approx(0.5f));
    CHECK(center(1) == Catch::Approx(1));
    CHECK(field.emitter(0).particles()[0].age == 1);
}

TEST_CASE("particle depth comparison remains independent of writes and batching",
          "[world][particles][vfx-depth]") {
    test::FakeRenderDevice device;
    ParticleField field;
    for (const bool compare : {true, false}) {
        for (const bool write : {true, false}) {
            ParticleDescriptor descriptor;
            descriptor.rate = {1, 1, 1, 1};
            descriptor.particleLife = 30;
            descriptor.emitFrames = 30;
            descriptor.depthTest = compare;
            descriptor.depthWrite = write;
            field.start(descriptor, Mat4{1}, &device.whiteTexture());
        }
    }
    field.step(1.0f / 30);
    REQUIRE(field.particleCount() == 4);
    field.draw(device, Mat4{1}, Vec3{1, 0, 0}, Vec3{0, 1, 0});
    REQUIRE(device.draws.size() == 4);
    for (usize i = 0; i < device.draws.size(); ++i) {
        CHECK(device.draws[i].state.depthTest == (i < 2));
        CHECK(device.draws[i].state.depthWrite == (i % 2 == 0));
    }
}

TEST_CASE("placed particle templates preserve an explicit always-pass depth flag",
          "[world][particles][vfx-depth]") {
    const auto dir = test::scratchDirectory("particle-field-depth");
    writeTextFile(dir / "world.json", R"({
      "objects":[{"name":"PSYSA","position":[0,0,0],"flags":2048}],
      "particles":[{"id":"A","flags":3072,"flagMask":3072,
        "enables":544,"particleLife":[1,0],"rate":[30,30,30,30]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    test::FakeRenderDevice device;
    TextureSet textures;
    ParticleField field;
    field.bind(layout, textures, device);
    field.step(1.0f / 30);
    REQUIRE(field.particleCount() == 1);
    field.draw(device, Mat4{1}, Vec3{1, 0, 0}, Vec3{0, 1, 0});
    REQUIRE(device.draws.size() == 1);
    CHECK_FALSE(device.draws.front().state.depthTest);
    CHECK_FALSE(device.draws.front().state.depthWrite);
}

TEST_CASE("particle emission and expiry are independent of render cadence", "[world][particles]") {
    const test::FakeRenderDevice device;
    ParticleDescriptor descriptor;
    descriptor.delay = 1;
    descriptor.emitFrames = 3;
    descriptor.fadeFrames = 4;
    descriptor.particleLife = 8;
    descriptor.particleFade = 5;
    descriptor.rate = {7, 3, 2, 0};
    for (const s32 rate : {15, 30, 60, 120}) {
        INFO(rate);
        ParticleField field;
        field.start(descriptor, Mat4{1}, &device.whiteTexture(), 123);
        for (s32 i = 0; i < rate / 5; ++i) {
            field.step(1.0f / static_cast<f32>(rate));
        }
        REQUIRE(field.emitter(0).age() == 5); // six elapsed ticks, one delayed
        REQUIRE(field.particleCount() == 16);
        field.stop(0);
        for (s32 i = 0; i < rate; ++i) {
            field.step(1.0f / static_cast<f32>(rate));
        }
        REQUIRE(field.particleCount() == 0);
    }
}

TEST_CASE("a field starts an emitter at every marker naming a template", "[world][particles]") {
    const auto dir = test::sampleLevel("particle-field");
    test::FakeRenderDevice device;
    TextureSet textures;
    TextureSet lender;
    WorldLayout layout;
    REQUIRE(textures.load(dir));
    REQUIRE(lender.load(test::sampleLender("particle-field-lender")));
    REQUIRE(layout.load(dir));
    const std::array<TextureSet*, 1> lenders{&lender};
    ParticleField field;
    field.bind(layout, textures, device, lenders);
    REQUIRE(field.size() == 1);
    REQUIRE(field.textureOf(0) == &lender.texture(device, 3)); // lent by name
    REQUIRE(field.emitter(0).descriptor().additive);
    REQUIRE(field.particleCount() == 0);

    // Whole frames only: a third of one waits, then two frames come at once.
    field.step(1.0f / 90.0f);
    REQUIRE(field.particleCount() == 0);
    field.step(2.0f / 30.0f - 1.0f / 90.0f + 1e-4f);
    REQUIRE(field.particleCount() == 2);
    const Particle& first = field.emitter(0).particles()[0];
    REQUIRE(first.origin.z > 69.0f); // at the marker, under the group
    REQUIRE(first.origin.z < 71.0f);

    field.draw(device, Mat4{1.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws[0].texture == &lender.texture(device, 3));
    REQUIRE(device.draws[0].vertices.size() == 12);
    REQUIRE(device.draws[0].state.blend == BlendMode::Additive);
    REQUIRE_FALSE(device.draws[0].state.depthWrite);
    REQUIRE_FALSE(device.draws[0].state.cullBack);

    field.clear();
    REQUIRE(field.size() == 0);
    device.draws.clear();
    field.draw(device, Mat4{1.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    REQUIRE(device.draws.empty());
}

TEST_CASE("an emitter can be started on its own, stopped and pruned", "[world][particles]") {
    test::FakeRenderDevice device;
    ParticleField field;
    ParticleTemplate t;
    t.enables =
        ParticleTemplate::kEmitterLife | ParticleTemplate::kParticleLife | ParticleTemplate::kRate;
    t.emitterLife = {-1.0f, 0.0f}; // endless
    t.particleLife = {0.1f, 0.0f}; // three frames
    t.rate = {30.0f, 30.0f, 30.0f, 30.0f};
    const ParticleDescriptor d = ParticleDescriptor::fromTemplate(t);
    const usize spark =
        field.start(d, glm::translate(Mat4{1.0f}, Vec3{1.0f, 2.0f, 3.0f}), &device.whiteTexture());
    REQUIRE(field.size() == 1);
    REQUIRE(field.active(spark));
    field.step(2.0f / 30.0f);
    REQUIRE(field.particleCount() == 2);
    REQUIRE(field.emitter(spark).particles()[0].origin == Vec3{1.0f, 2.0f, 3.0f});
    // Moved, the next particles leave from the new place.
    field.setNode(spark, glm::translate(Mat4{1.0f}, Vec3{4.0f, 5.0f, 6.0f}));
    REQUIRE(Vec3{field.emitter(spark).node()[3]} == Vec3{4.0f, 5.0f, 6.0f});
    field.step(1.0f / 30.0f);
    REQUIRE(field.particleCount() == 3);
    REQUIRE(field.emitter(spark).particles()[2].origin == Vec3{4.0f, 5.0f, 6.0f});
    field.setNode(9, Mat4{1.0f}); // out of range: ignored
    field.draw(device, Mat4{1.0f}, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    REQUIRE(device.draws.size() == 1);
    REQUIRE(device.draws[0].texture == &device.whiteTexture());
    // Stopped, it lets its particles die, then it can be pruned away.
    field.stop(spark);
    field.prune();
    REQUIRE(field.size() == 1); // still showing its last particles
    field.step(4.0f / 30.0f);
    REQUIRE(field.particleCount() == 0);
    REQUIRE_FALSE(field.active(spark));
    field.prune();
    REQUIRE(field.size() == 0);
    field.stop(9); // out of range: ignored
}

TEST_CASE("markers without a template or texture are skipped or drawn white",
          "[world][particles]") {
    const auto dir = test::scratchDirectory("particle-field-odd");
    writeTextFile(dir / "world.json", R"({
  "objects": [
    {"name": "L1PSYSQ_LOST", "position": [0, 0, 0], "next": 1, "flags": 2048},
    {"name": "NOTATAG", "position": [0, 0, 0], "next": 2, "flags": 2048},
    {"name": "PSYS", "position": [0, 0, 0], "next": 3, "flags": 2048},
    {"name": "PSYSZ", "position": [1, 2, 3], "next": -1, "flags": 2048}
  ],
  "particles": [
    {"id": "Z", "preset": 5, "enables": 16385, "texture": "NOWHERE"}
  ]
})");
    test::FakeRenderDevice device;
    TextureSet textures;
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    ParticleField field;
    field.bind(layout, textures, device);
    REQUIRE(field.size() == 1);
    REQUIRE(field.textureOf(0) == &device.whiteTexture());
    REQUIRE(field.emitter(0).descriptor().speed == 1.5f); // the preset's
}

TEST_CASE("named world emitters repeat while independently started effects expire",
          "[world][particles][visual-parity]") {
    const auto dir = test::scratchDirectory("particle-field-repeat");
    writeTextFile(dir / "world.json", R"({
      "objects":[{"name":"PSYSA","position":[0,0,0],"flags":2048}],
      "particles":[{"id":"A","enables":560,"emitterLife":[0.1,0.1],
        "particleLife":[0.1,0],"rate":[30,30,0,0]}]})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    test::FakeRenderDevice device;
    TextureSet textures;
    ParticleField world;
    world.bind(layout, textures, device);
    REQUIRE(world.size() == 1);
    CHECK(world.emitter(0).descriptor().forever);
    ParticleField effects;
    const auto descriptor = ParticleDescriptor::fromTemplate(*layout.findParticleTemplate('A'));
    REQUIRE_FALSE(descriptor.forever);
    effects.start(descriptor, Mat4{1}, &device.whiteTexture());
    usize lateBirths = 0;
    for (s32 frame = 0; frame < 300; ++frame) {
        world.step(1.0f / 30);
        effects.step(1.0f / 30);
        if (frame > 270 && world.particleCount() > 0) {
            ++lateBirths;
        }
    }
    CHECK(lateBirths > 0);
    CHECK(world.active(0));
    CHECK_FALSE(effects.active(0));
}

TEST_CASE("tower mountain sparks keep erupting after the arrival camera finishes",
          "[world][particles][visual-parity][assets]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    TextureSet textures;
    REQUIRE(layout.load(directory));
    REQUIRE(textures.load(directory));
    TextureSet items;
    REQUIRE(items.load(test::assetOrSkip("ITEMS/LEVELL/textures.ngc").parent_path()));
    const std::array<TextureSet*, 1> lenders{&items};
    test::FakeRenderDevice device;
    ParticleField field;
    field.bind(layout, textures, device, lenders);
    usize sparks = 0;
    for (usize i = 0; i < field.size(); ++i) {
        if (field.emitter(i).descriptor().texture == "EMBER_SPARK2") {
            ++sparks;
            REQUIRE(field.emitter(i).descriptor().forever);
            REQUIRE(field.textureOf(i) != &device.whiteTexture());
        }
    }
    REQUIRE(sparks == 5);
    usize lateDraws = 0;
    for (s32 frame = 0; frame < 1200; ++frame) {
        field.step(1.0f / 30);
        if (frame < 900) {
            continue;
        }
        for (usize i = 0; i < field.size(); ++i) {
            if (field.emitter(i).descriptor().texture == "EMBER_SPARK2" &&
                !field.emitter(i).particles().empty()) {
                ++lateDraws;
            }
        }
    }
    CHECK(lateDraws > 300);
}

} // namespace
