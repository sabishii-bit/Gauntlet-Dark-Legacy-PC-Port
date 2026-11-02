#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <typeindex>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"
#include "engine/world/ParticleField.h"

#include "FakeRenderDevice.h"
#include "SampleLevel.h"

namespace {
using namespace gdl;

// TextureSet retains these ordinary independently allocated textures. Ordering
// their delivery explicitly exercises the production pointer batching sort;
// allocator luck cannot make the two worlds accidentally use the same order.
class ParticleTextureOrderDevice final : public RenderDevice {
public:
    explicit ParticleTextureOrderDevice(bool reverse) {
        for (s32 i = 0; i < 2; ++i) {
            m_textures.push_back(std::make_unique<test::FakeTexture>(2, 2));
        }
        std::ranges::sort(m_textures, [](const auto& a, const auto& b) {
            return std::less<const Texture*>{}(a.get(), b.get());
        });
        if (reverse) {
            std::ranges::reverse(m_textures);
        }
    }
    bool beginFrame() override { return true; }
    void endFrame() override {}
    void setClearColor(const Vec4& /*rgba*/) override {}
    Extent2D framebufferExtent() const override { return {640, 448}; }
    void setPresentation(bool /*vsync*/, u32 /*sampleCount*/) override {}
    u32 presentationSampleCount() const override { return 1; }
    void setTextureFiltering(u32 /*filtering*/) override {}
    void setSmoothSprites(bool /*enabled*/) override {}
    void setAuthoredSilhouettes(bool /*enabled*/) override {}
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                           std::span<const u8> pixels) override {
        REQUIRE(desc.width == 2);
        REQUIRE(desc.height == 2);
        REQUIRE(m_next < m_textures.size());
        m_textures[m_next]->pixels.assign(pixels.begin(), pixels.end());
        return std::move(m_textures[m_next++]);
    }
    void updateTexture(Texture& /*texture*/, std::span<const u8> /*rgba8Pixels*/) override {
        FAIL("unexpected upload");
    }
    const Texture& whiteTexture() const override { return m_white; }
    void draw(const ImmediateBatch& /*batch*/, const Texture& /*texture*/,
              const Mat4& /*transform*/, const DrawState& /*state*/) override {}
    void waitIdle() override {}
    bool applyDepthOfField(const DepthOfField& /*settings*/) override { return false; }
    bool applyBloom() override { return false; }
    void addHeatSource(const HeatSource& /*source*/) override {}
    bool applyAmbientOcclusion(const AmbientOcclusion& /*settings*/) override { return false; }

private:
    test::FakeTexture m_white{1, 1};
    std::vector<std::unique_ptr<test::FakeTexture>> m_textures;
    usize m_next = 0;
};

struct ParticleTextureBindings {
    std::array<const Texture*, 3> textures;
    static bool append(NativeStateDigest& digest, std::type_index type, const void* pointer,
                       void* context) {
        const auto& bindings = *static_cast<const ParticleTextureBindings*>(context);
        if (type != typeid(Texture)) {
            return false;
        }
        for (usize i = 0; i < bindings.textures.size(); ++i) {
            if (pointer == bindings.textures[i]) {
                digest.string("particle-order-fixture/texture");
                digest.wide(i);
                return true;
            }
        }
        return false;
    }
    NativeStateDigest::Digest hash(const ParticleField& field) {
        NativeStateDigest digest({this, append});
        digest.value(field);
        REQUIRE(digest.checkedFinish());
        return *digest.checkedFinish();
    }
};

TEST_CASE("native particle digest canonicalizes only authored texture-batched markers",
          "[native-digest][particle-native-digest][particle-local-rewind]") {
    const auto directory = test::sampleLevel("particle-native-digest-texture-order");
    writeTextFile(directory / "world.json", R"({
      "objects":[{"name":"ROOT","position":[0,0,0],"child":1},
        {"name":"PSYSA","position":[1,2,3],"flags":2048,"next":2},
        {"name":"PSYSB","position":[4,5,6],"flags":2048}],
      "particles":[
        {"id":"A","enables":25248,"particleLife":[3,0],"rate":[30,30,30,30],
         "direction":[1,0,0],"speed":30,"texture":"STONE"},
        {"id":"B","enables":25248,"particleLife":[3,0],"rate":[30,30,30,30],
         "direction":[0,1,0],"speed":15,"texture":"GLASS"}]})");
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    ParticleTextureOrderDevice ascending(false);
    ParticleTextureOrderDevice descending(true);
    TextureSet firstTextures;
    TextureSet secondTextures;
    REQUIRE(firstTextures.load(directory));
    REQUIRE(secondTextures.load(directory));
    ParticleField first;
    ParticleField second;
    first.bind(layout, firstTextures, ascending, {}, 871);
    second.bind(layout, secondTextures, descending, {}, 871);
    REQUIRE(first.size() == 2);
    REQUIRE(second.size() == 2);
    REQUIRE(first.sourceObject(0) == 1);
    REQUIRE(second.sourceObject(0) == 2);
    REQUIRE(first.sourceObject(1) == 2);
    REQUIRE(second.sourceObject(1) == 1);
    REQUIRE(std::less<const Texture*>{}(first.textureOf(0), first.textureOf(1)));
    REQUIRE(std::less<const Texture*>{}(second.textureOf(0), second.textureOf(1)));
    REQUIRE(first.textureOf(0) != second.textureOf(1));
    ParticleTextureBindings firstBindings{
        {first.textureOf(0), first.textureOf(1), &ascending.whiteTexture()}};
    ParticleTextureBindings secondBindings{
        {second.textureOf(1), second.textureOf(0), &descending.whiteTexture()}};
    const auto firstOrder = std::array{first.sourceObject(0), first.sourceObject(1)};
    const auto secondOrder = std::array{second.sourceObject(0), second.sourceObject(1)};
    CHECK(firstBindings.hash(first) == secondBindings.hash(second));
    for (u32 tick = 0; tick < 8; ++tick) {
        first.step(1.0f / 60);
        second.step(1.0f / 60);
        CHECK(firstBindings.hash(first) == secondBindings.hash(second));
    }

    // Independent effects keep their index meanings even when appended after a
    // differently batched set of world markers. Reversing those two is not equal.
    auto reversedIndependent = second;
    const auto descriptor = first.emitter(0).descriptor();
    const auto add = [&](ParticleField& field, const Texture& texture, bool reverse) {
        for (s32 i = 0; i < 2; ++i) {
            const s32 index = reverse ? 1 - i : i;
            field.start(descriptor,
                        glm::translate(Mat4{1}, Vec3{10.0f + static_cast<f32>(index), 2, 3}),
                        &texture, static_cast<u32>(index + 23));
        }
    };
    add(first, ascending.whiteTexture(), false);
    add(second, descending.whiteTexture(), false);
    add(reversedIndependent, descending.whiteTexture(), true);
    CHECK(firstBindings.hash(first) == secondBindings.hash(second));
    CHECK(secondBindings.hash(reversedIndependent) != secondBindings.hash(second));
    const auto saved = second;
    const auto expected = secondBindings.hash(saved);
    second.setNode(1, glm::translate(Mat4{1}, Vec3{9, 8, 7}));
    CHECK(secondBindings.hash(second) != expected);
    second = saved;
    second.setTextureBlend(0, descending.whiteTexture(), second.textureOf(1), 0.37f);
    second.draw(descending, Mat4{1}, {1, 0, 0}, {0, 1, 0}, 0.37f, {});
    CHECK(secondBindings.hash(second) == expected);
    CHECK(std::array{first.sourceObject(0), first.sourceObject(1)} == firstOrder);
    CHECK(std::array{second.sourceObject(0), second.sourceObject(1)} == secondOrder);
    NativeStateDigest unresolved;
    unresolved.value(second);
    CHECK_FALSE(unresolved.checkedFinish());
}

TEST_CASE("native particle value copies restore topology RNG and fractional clocks after draws",
          "[local-rewind][particle-local-rewind]") {
    test::FakeRenderDevice device;
    ParticleDescriptor descriptor;
    descriptor.forever = true;
    descriptor.emitFrames = 9;
    descriptor.fadeFrames = 5;
    descriptor.particleLife = 15;
    descriptor.particleFade = 3;
    descriptor.maxParticles = 80;
    descriptor.rate = {4, 2, 1, 0};
    descriptor.rateRandom = 0.5f;
    descriptor.volume = {2, 1, 3};
    descriptor.angle = 1;
    descriptor.speed = 0.3f;
    descriptor.gravity = -0.02f;
    descriptor.alpha = {255, 180, 180, 0};
    descriptor.width = {1, 2, 2, 0};
    descriptor.depthWrite = false;
    ParticleField reference;
    reference.start(descriptor, Mat4{1}, &device.whiteTexture(), 1287);
    ParticleField predicted = reference;
    std::array<ParticleField, 16> history;
    for (u32 frame = 0; frame < 192; ++frame) {
        const u32 distance = frame % 8 + 1;
        CAPTURE(frame, distance);
        predicted.draw(device, Mat4{1}, {1, 0, 0}, {0, 1, 0}, 0.37f, {});
        history[frame % history.size()] = predicted;
        for (u32 i = 0; i < distance; ++i) {
            predicted.setNode(0, glm::translate(Mat4{1}, Vec3{9, 8, 7}));
            predicted.step(1.0f / 60);
        }
        predicted.start(descriptor, Mat4{1}, &device.whiteTexture(), frame + 9);
        predicted.stop(0);
        predicted.prune();
        predicted = history[frame % history.size()];
        REQUIRE(predicted.size() == 1);
        CHECK(predicted.equalLocal(reference));
        CHECK(predicted.checkpoint() == reference.checkpoint());
        for (u32 i = 0; i < distance; ++i) {
            const Mat4 node = glm::translate(Mat4{1}, Vec3{static_cast<f32>(i), 2, 3});
            reference.setNode(0, node);
            predicted.setNode(0, node);
            reference.step(1.0f / 60);
            predicted.step(1.0f / 60);
            CHECK(predicted.equalLocal(reference));
            CHECK(predicted.checkpoint() == reference.checkpoint());
        }
        device.draws.clear();
        reference.draw(device, Mat4{1}, {1, 0, 0}, {0, 1, 0}, 0.37f, {});
        const auto expected = device.draws;
        device.draws.clear();
        predicted.draw(device, Mat4{1}, {1, 0, 0}, {0, 1, 0}, 0.37f, {});
        REQUIRE(device.draws.size() == expected.size());
        for (usize d = 0; d < expected.size(); ++d) {
            CHECK(device.draws[d].state == expected[d].state);
            CHECK(device.draws[d].texture == expected[d].texture);
            REQUIRE(device.draws[d].vertices.size() == expected[d].vertices.size());
            for (usize v = 0; v < expected[d].vertices.size(); ++v) {
                const auto& a = device.draws[d].vertices[v];
                const auto& b = expected[d].vertices[v];
                CHECK(a.position == b.position);
                CHECK(a.color == b.color);
                CHECK(a.uv == b.uv);
            }
        }
        device.draws.clear();
    }
    CHECK(device.texturesCreated == 0);
    CHECK(device.textureUpdates == 0);
}

TEST_CASE("stepped particle draw samples preserve native bindings and do not blend cutouts",
          "[native-digest][particle-native-digest][particle-local-rewind][presentation]") {
    test::FakeRenderDevice device;
    test::FakeTexture shown{2, 2};
    test::FakeTexture next{2, 2};
    ParticleDescriptor descriptor;
    descriptor.oneShot = true;
    descriptor.maxParticles = 1;
    descriptor.particleLife = 30;
    descriptor.width = {1, 1, 1, 1};
    descriptor.depthWrite = true;
    descriptor.additive = false;
    ParticleField field;
    field.start(descriptor, Mat4{1}, &device.whiteTexture());
    field.step(1.0f / 30);
    field.step(1.0f / 30); // Visible at both ends of the interpolation interval.
    REQUIRE(field.particleCount() == 1);
    REQUIRE(field.emitter(0).particles()[0].age >= 1);
    const auto before = field;
    ParticleTextureBindings bindings{{&device.whiteTexture(), &shown, &next}};
    const auto baseline = bindings.hash(field);
    for (const f32 alpha : {-1.0f, 0.0f, 0.5f, 1.0f}) {
        CAPTURE(alpha);
        field.setTextureBlend(0, shown, &next, 0.8f, true);
        device.draws.clear();
        field.draw(device, Mat4{1}, {1, 0, 0}, {0, 1, 0}, alpha, {});
        REQUIRE(device.draws.size() == 1);
        CHECK(device.draws[0].texture == &shown);
        CHECK(device.draws[0].state.nextTexture == nullptr);
        CHECK(device.draws[0].state.textureBlend == 0.0f);
        CHECK(field.textureOf(0) == &device.whiteTexture());
        CHECK(field.equalLocal(before));
        CHECK(bindings.hash(field) == baseline);
    }
    // The old interpolated-only API still ignores overrides on hard cutouts.
    field.setTextureBlend(0, shown, &next, 0.8f);
    device.draws.clear();
    field.draw(device, Mat4{1}, {1, 0, 0}, {0, 1, 0}, 0.5f, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].texture == &device.whiteTexture());
    field.setTextureBlend(0, shown, &next, 0.8f, true);
    field.clearTextureBlends();
    device.draws.clear();
    field.draw(device, Mat4{1}, {1, 0, 0}, {0, 1, 0}, -1, {});
    REQUIRE(device.draws.size() == 1);
    CHECK(device.draws[0].texture == &device.whiteTexture());
    CHECK(bindings.hash(field) == baseline);
}

TEST_CASE("native particle comparison sees fractional and hidden emission state not draw caches",
          "[local-rewind][particle-local-rewind]") {
    test::FakeRenderDevice device;
    ParticleDescriptor descriptor;
    descriptor.delay = 5;
    descriptor.emitFrames = 20;
    descriptor.rate = {1, 1, 1, 1};
    ParticleField base;
    base.start(descriptor, Mat4{1}, &device.whiteTexture(), 211);
    auto changed = base;
    REQUIRE(changed.equalLocal(base));
    changed.step(1.0f / 120);
    CHECK(changed.particleCount() == 0);
    CHECK_FALSE(changed.equalLocal(base));
    changed = base;
    changed.setEmitting(0, false);
    CHECK_FALSE(changed.equalLocal(base));
    changed = base;
    changed.setSpriteScale(0, 0.5f);
    CHECK_FALSE(changed.equalLocal(base));
    changed = base;
    changed.setTextureBlend(0, device.whiteTexture(), &device.whiteTexture(), 0.5f);
    changed.draw(device, Mat4{1}, {1, 0, 0}, {0, 1, 0}, 0.5f, {});
    CHECK(changed.equalLocal(base));

    ParticleEmitter a;
    a.start(descriptor, Mat4{1}, 211);
    auto b = a;
    REQUIRE(a.equalLocal(b));
    b.start(descriptor, Mat4{1}, 212);
    CHECK_FALSE(a.equalLocal(b)); // Same phase/geometry, different next random draw.
}
} // namespace
