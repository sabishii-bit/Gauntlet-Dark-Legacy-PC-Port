#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/io/File.h"
#include "engine/world/ParticleField.h"

#include "FakeRenderDevice.h"
#include "SampleLevel.h"
#include "TestSupport.h"

namespace {

using namespace gdl;

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
    t.enables = ParticleTemplate::kEmitterLife | ParticleTemplate::kParticleLife |
                ParticleTemplate::kRate;
    t.emitterLife = {-1.0f, 0.0f}; // endless
    t.particleLife = {0.1f, 0.0f}; // three frames
    t.rate = {30.0f, 30.0f, 30.0f, 30.0f};
    const ParticleDescriptor d = ParticleDescriptor::fromTemplate(t);
    const usize spark = field.start(d, glm::translate(Mat4{1.0f}, Vec3{1.0f, 2.0f, 3.0f}),
                                    &device.whiteTexture());
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

} // namespace
