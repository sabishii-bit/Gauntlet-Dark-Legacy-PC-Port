#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ParticleTemplate.h"
#include "engine/core/Types.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/world/ParticleSystem.h"

namespace {

using namespace gdl;
using Catch::Approx;

/** The tower's brazier flame: preset 5 with the level's own life, cone, volume, rate,
 * lift, speed, colours and widths, added onto the frame without writing depth. */
ParticleTemplate torchTemplate() {
    ParticleTemplate t;
    t.id = 'E';
    t.preset = 5;
    t.flags = 0x288;
    t.flagMask = 0x288;
    t.enables = 0x56BE1;
    t.particleLife = {0.2f, 0.22f};
    t.angle = 80.0f;
    t.texture = "p_torch";
    t.direction = Vec3{0.0f, 1.0f, 0.0f};
    t.volume = Vec3{0.1f, 0.3f, 0.1f};
    t.rate = {25.0f, 25.0f, 25.0f, 25.0f};
    t.gravity = -0.38f;
    t.speed = 4.0f;
    t.rgba = {0, 0x00FFFFFF, 0x00FFFFFF, 0};
    t.width = {2.8f, 2.0f, 2.0f, 0.1f};
    return t;
}

TEST_CASE("an envelope runs from birth to the life's end, then over the fade",
          "[world][particles]") {
    const ParticleEnvelope envelope{0.0f, 100.0f, 50.0f, 10.0f};
    REQUIRE(envelope.at(0.0f, 10.0f, 4.0f) == 0.0f);
    REQUIRE(envelope.at(5.0f, 10.0f, 4.0f) == 50.0f);
    REQUIRE(envelope.at(10.0f, 10.0f, 4.0f) == 50.0f);
    REQUIRE(envelope.at(12.0f, 10.0f, 4.0f) == 30.0f);
    REQUIRE(envelope.at(14.0f, 10.0f, 4.0f) == 10.0f);
    REQUIRE(envelope.at(20.0f, 10.0f, 4.0f) == 10.0f);
    REQUIRE(envelope.at(11.0f, 10.0f, 0.0f) == 10.0f);
}

TEST_CASE("zero-speed cone particles stay finite and rise under gravity", "[world][particles]") {
    auto source = torchTemplate();
    source.speed = 0.0f;
    source.angle = 5.0f;
    const auto descriptor = ParticleDescriptor::fromTemplate(source);
    ParticleEmitter emitter;
    emitter.start(descriptor, glm::translate(Mat4{1.0f}, Vec3{10, 20, 30}));
    emitter.step(1);
    REQUIRE_FALSE(emitter.particles().empty());
    const auto born = emitter.particles()[0];
    REQUIRE(born.velocity == Vec3{0.0f});
    emitter.step(2);
    const auto& particle = emitter.particles()[0];
    const auto position = emitter.positionOf(particle);
    REQUIRE(std::isfinite(position.x));
    REQUIRE(std::isfinite(position.y));
    REQUIRE(std::isfinite(position.z));
    REQUIRE(position.x == born.origin.x);
    REQUIRE(position.z == born.origin.z);
    REQUIRE(position.y > born.origin.y);
}

TEST_CASE("a template resolves over its preset into frames and units", "[world][particles]") {
    REQUIRE(particlePresets().size() == 8);
    REQUIRE(particlePresets()[5].preset == 5);
    REQUIRE(particlePresets()[5].speed == 45.0f);
    const ParticleDescriptor d = ParticleDescriptor::fromTemplate(torchTemplate());
    // The preset's 999 seconds of emitting and fading, alpha and no depth write stand.
    REQUIRE(d.emitFrames == 29970);
    REQUIRE(d.fadeFrames == 29970);
    REQUIRE_FALSE(d.forever);
    REQUIRE_FALSE(d.oneShot);
    REQUIRE(d.alpha.lifeStart == 255.0f);
    REQUIRE(d.alpha.fadeEnd == 255.0f);
    REQUIRE_FALSE(d.depthWrite);
    // The level's own fields override it.
    REQUIRE(d.particleLife == 6);
    REQUIRE(d.particleFade == 6);
    REQUIRE(d.angle == Approx(kPi * 80.0f / 360.0f));
    REQUIRE(d.direction == Vec3{0.0f, 1.0f, 0.0f});
    REQUIRE(d.volume == Vec3{0.1f, 0.3f, 0.1f});
    REQUIRE(d.rate[0] == Approx(25.0f / 30.0f));
    REQUIRE(d.rateRandom == Approx(0.01f));
    REQUIRE(d.gravity == Approx(0.38f * 32.0f / 900.0f)); // lifted, not dropped
    REQUIRE(d.speed == Approx(4.0f / 30.0f));
    REQUIRE(d.red.lifeStart == 0.0f);
    REQUIRE(d.red.lifeEnd == 255.0f);
    REQUIRE(d.green.fadeStart == 255.0f);
    REQUIRE(d.blue.fadeEnd == 0.0f);
    REQUIRE(d.width.lifeStart == 2.8f);
    REQUIRE(d.width.fadeEnd == 0.1f);
    REQUIRE(d.texture == "P_TORCH");
    REQUIRE(d.additive);
    REQUIRE(d.sorted);
    REQUIRE(d.capacity() == 10);

    // A template on its own: the angle's edges, endless phases and the default gravity.
    ParticleTemplate bare;
    bare.enables =
        ParticleTemplate::kAngle | ParticleTemplate::kEmitterLife | ParticleTemplate::kMaxParticles;
    bare.angle = 360.0f;
    bare.emitterLife = {-1.0f, 2.0f};
    bare.maxParticles = 40;
    bare.flags = ParticleTemplate::kDefaultGravity;
    const ParticleDescriptor sphere = ParticleDescriptor::fromTemplate(bare);
    REQUIRE(sphere.angle == ParticleDescriptor::kSphere);
    REQUIRE(sphere.emitFrames == ParticleDescriptor::kEndless);
    REQUIRE(sphere.forever);
    REQUIRE(sphere.gravity == Approx(-32.0f / 900.0f));
    REQUIRE(sphere.capacity() == 40);
    bare.angle = 0.5f;
    bare.emitterLife = {1.0f, -1.0f};
    REQUIRE(ParticleDescriptor::fromTemplate(bare).angle == 0.0f);
    REQUIRE(ParticleDescriptor::fromTemplate(bare).fadeFrames == ParticleDescriptor::kEndless);
}

TEST_CASE("an emitter lets particles out at its rate, lifts them and lets them die",
          "[world][particles]") {
    const ParticleDescriptor d = ParticleDescriptor::fromTemplate(torchTemplate());
    ParticleEmitter emitter;
    REQUIRE_FALSE(emitter.active());
    const Mat4 node = glm::translate(Mat4{1.0f}, Vec3{10.0f, 5.0f, -20.0f});
    emitter.start(d, node, 7);
    REQUIRE(emitter.active());
    REQUIRE(emitter.phase() == ParticleEmitter::Phase::Emitting);
    REQUIRE(emitter.particles().empty());

    emitter.step(1);
    REQUIRE(emitter.particles().size() == 1);
    const Particle& first = emitter.particles()[0];
    REQUIRE(first.age == 0.0f);
    REQUIRE(std::abs(first.origin.x - 10.0f) <= 0.05f);
    REQUIRE(std::abs(first.origin.y - 5.0f) <= 0.15f);
    REQUIRE(std::abs(first.origin.z + 20.0f) <= 0.05f);
    // Within the cone about up, at the speed.
    REQUIRE(glm::length(first.velocity) == Approx(4.0f / 30.0f).margin(1e-4f));
    REQUIRE(first.velocity.y > 0.0f);
    REQUIRE(emitter.positionOf(first) == first.origin);
    REQUIRE(emitter.colorOf(first) == Color::rgba(0, 0, 0, 255)); // born dark
    REQUIRE(emitter.widthOf(first) == 2.8f);

    // Six frames on: ten particles at most, the first bright, wide and lifted.
    for (s32 i = 0; i < 5; ++i) {
        emitter.step(1);
    }
    REQUIRE(emitter.particles().size() == 5);
    const Particle& oldest = emitter.particles()[0];
    REQUIRE(oldest.age == 5.0f);
    REQUIRE(emitter.colorOf(oldest).r == 212);
    REQUIRE(emitter.widthOf(oldest) == Approx(2.8f - 5.0f * 0.8f / 6.0f));
    const Vec3 lifted = emitter.positionOf(oldest);
    REQUIRE(lifted.y > oldest.origin.y + 5.0f * oldest.velocity.y);
    // At the end of its fade it is gone.
    emitter.step(7);
    REQUIRE(emitter.particles().size() == 10);
    for (const Particle& particle : emitter.particles()) {
        REQUIRE(particle.age < 12.0f);
    }
    // A long gap counts as one frame.
    emitter.step(40);
    REQUIRE(emitter.age() == 14);

    // Drawn as squares facing the camera.
    ImmediateBatch batch;
    batch.begin(PrimitiveTopology::TriangleList);
    emitter.draw(batch, Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    batch.end();
    REQUIRE(batch.triangles().size() == emitter.particles().size() * 6);
    const ImmediateVertex& corner = batch.triangles()[1];
    REQUIRE(corner.uv == Vec2{1.0f, 0.0f});
    REQUIRE(batch.triangles()[0].position.x < corner.position.x);
    REQUIRE(batch.triangles()[0].position.y == corner.position.y);
    REQUIRE(batch.triangles()[5].position.y < corner.position.y);
}

TEST_CASE("an emitter's phases run their frames, then it stops or starts over",
          "[world][particles]") {
    ParticleTemplate t;
    t.enables = ParticleTemplate::kEmitterLife | ParticleTemplate::kParticleLife |
                ParticleTemplate::kRate | ParticleTemplate::kDelay;
    t.emitterLife = {0.1f, 0.2f}; // three frames emitting, six fading
    t.particleLife = {0.1f, 0.0f};
    t.rate = {30.0f, 30.0f, 60.0f, 0.0f};
    t.delay = 0.1f;
    ParticleDescriptor d = ParticleDescriptor::fromTemplate(t);
    REQUIRE(d.delay == 3);
    REQUIRE(d.emitFrames == 3);
    REQUIRE(d.fadeFrames == 6);
    REQUIRE(d.particleLife == 3);
    ParticleEmitter emitter;
    emitter.start(d, Mat4{1.0f});
    REQUIRE(emitter.phase() == ParticleEmitter::Phase::Delay);
    emitter.step(3);
    REQUIRE(emitter.phase() == ParticleEmitter::Phase::Delay);
    REQUIRE(emitter.particles().empty());
    emitter.step(1); // one frame into emitting: one particle a frame
    REQUIRE(emitter.phase() == ParticleEmitter::Phase::Emitting);
    REQUIRE(emitter.particles().size() == 1);
    emitter.step(3); // into the fade, where the rate runs down from two a frame
    REQUIRE(emitter.phase() == ParticleEmitter::Phase::Fading);
    REQUIRE(emitter.particles().size() == 4); // two old births survive, two born on the fade tick
    emitter.step(3);
    emitter.step(1);
    emitter.step(1);
    REQUIRE(emitter.phase() == ParticleEmitter::Phase::Fading);
    emitter.step(1);
    REQUIRE(emitter.phase() == ParticleEmitter::Phase::Done);
    REQUIRE_FALSE(emitter.active()); // the final fade births have now lived their three frames
    emitter.step(4);
    REQUIRE_FALSE(emitter.active());

    // Forever, the phases come round again.
    d.forever = true;
    d.delay = 0;
    emitter.start(d, Mat4{1.0f});
    emitter.step(10);
    REQUIRE(emitter.phase() == ParticleEmitter::Phase::Emitting);
    REQUIRE(emitter.age() == 0);

    // One shot: everything at once, then nothing more.
    d.forever = false;
    d.oneShot = true;
    d.maxParticles = 12;
    emitter.start(d, Mat4{1.0f});
    emitter.step(1);
    REQUIRE(emitter.particles().size() == 12);
    REQUIRE(emitter.phase() == ParticleEmitter::Phase::Done);
    emitter.step(1);
    REQUIRE(emitter.particles().size() == 12);
}

TEST_CASE("particle batches preserve every birth across phase boundaries", "[world][particles]") {
    for (const bool oneShot : {false, true}) {
        for (const bool forever : {false, true}) {
            ParticleDescriptor descriptor = ParticleDescriptor::fromTemplate(torchTemplate());
            descriptor.delay = 2;
            descriptor.emitFrames = 3;
            descriptor.fadeFrames = 6;
            descriptor.rate = {6, 0, 2, 0};
            descriptor.oneShot = oneShot;
            descriptor.forever = forever;
            descriptor.maxParticles = 30;
            ParticleEmitter single;
            ParticleEmitter batched;
            single.start(descriptor, Mat4{1}, 123);
            batched.start(descriptor, Mat4{1}, 123);
            for (const u32 frames : {4U, 7U, 15U, 2U, 40U}) {
                INFO(oneShot << " " << forever << " " << frames);
                const u32 ticks = frames > ParticleEmitter::kMostFramesAtOnce ? 1 : frames;
                for (u32 i = 0; i < ticks; ++i) {
                    single.step(1);
                }
                batched.step(frames);
                REQUIRE(single.phase() == batched.phase());
                REQUIRE(single.age() == batched.age());
                REQUIRE(single.particles().size() == batched.particles().size());
                for (usize i = 0; i < single.particles().size(); ++i) {
                    CHECK(single.particles()[i].age == batched.particles()[i].age);
                    CHECK(single.particles()[i].origin == batched.particles()[i].origin);
                    CHECK(single.particles()[i].velocity == batched.particles()[i].velocity);
                }
            }
        }
    }
}

TEST_CASE("emission gates preserve live tails and clocks without accumulating paused births",
          "[world][particles]") {
    ParticleDescriptor descriptor;
    descriptor.emitFrames = ParticleDescriptor::kEndless;
    descriptor.rate = {1, 1, 1, 1};
    descriptor.particleLife = 10;
    ParticleEmitter emitter;
    emitter.start(descriptor, Mat4{1});
    emitter.step(1);
    REQUIRE(emitter.particles().size() == 1);
    emitter.setEmitting(false);
    emitter.step(3);
    REQUIRE(emitter.age() == 4);
    REQUIRE(emitter.particles().size() == 1);
    REQUIRE(emitter.particles()[0].age == 3);
    emitter.setEmitting(true);
    emitter.step(1);
    REQUIRE(emitter.particles().size() == 2);
    emitter.finish();
    emitter.step(10);
    REQUIRE_FALSE(emitter.active());
}

TEST_CASE("particle sprite width uses explicit local scale rather than inherited matrix scale",
          "[world][particles]") {
    ParticleDescriptor descriptor;
    descriptor.oneShot = true;
    descriptor.maxParticles = 1;
    descriptor.particleLife = 10;
    descriptor.width = {2, 2, 2, 2};
    ParticleEmitter emitter;
    emitter.start(descriptor, glm::scale(Mat4{1}, Vec3{7}));
    emitter.step(1);
    for (const f32 scale : {1.0f, 3.0f}) {
        emitter.setSpriteScale(scale);
        ImmediateBatch batch;
        batch.begin(PrimitiveTopology::TriangleList);
        emitter.draw(batch, Vec3{1, 0, 0}, Vec3{0, 1, 0});
        batch.end();
        REQUIRE(batch.triangles().size() == 6);
        CHECK(batch.triangles()[1].position.x - batch.triangles()[0].position.x == 2 * scale);
    }
}

} // namespace
