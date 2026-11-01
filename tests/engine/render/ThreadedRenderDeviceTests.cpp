#include <array>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "engine/core/Clock.h"
#include "engine/render/ThreadedRenderDevice.h"

#include "FakeRenderDevice.h"

namespace {
using namespace gdl;

struct Control {
    std::mutex mutex;
    std::condition_variable changed;
    bool blocked = false;
    bool blockAtEnd = false;
    bool entered = false;
    bool skip = false;
    bool fail = false;
    bool destroyed = false;
    std::thread::id renderThread;
    std::thread::id destroyThread;
    std::thread::id detachThread;
    std::thread::id attachThread;
    s32 detached = 0;
    s32 attached = 0;
    test::FakeRenderDevice draws;
    Extent2D extent;
    std::array<std::vector<u8>, 4> sampled;
    std::vector<std::string> events;

    bool waitForEntry() {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    void release() {
        {
            const std::lock_guard lock(mutex);
            blocked = false;
        }
        changed.notify_one();
    }
    void gate() {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_one();
        changed.wait(lock, [&] { return !blocked; });
    }
};

class Backend final : public RenderDevice {
public:
    GDL_NON_COPYABLE_NON_MOVABLE(Backend);
    explicit Backend(std::shared_ptr<Control> control) : m_control(std::move(control)) {}
    ~Backend() override {
        m_control->destroyThread = std::this_thread::get_id();
        m_control->destroyed = true;
    }
    bool beginFrame() override {
        auto& control = *m_control;
        control.renderThread = std::this_thread::get_id();
        if (!control.blockAtEnd) {
            control.gate();
        }
        if (control.fail) {
            throw std::runtime_error("test presentation failure");
        }
        control.events.emplace_back("begin");
        return !control.skip && control.draws.beginFrame();
    }
    void endFrame() override {
        if (m_control->blockAtEnd) {
            m_control->gate();
        }
        m_control->events.emplace_back("end");
    }
    void setClearColor(const Vec4& color) override { m_control->draws.setClearColor(color); }
    Extent2D framebufferExtent() const override { return m_control->extent; }
    void setFramebufferSize(Extent2D size) override { m_control->extent = size; }
    void setPresentation(bool vsync, u32 samples) override {
        m_control->draws.setPresentation(vsync, samples);
    }
    u32 presentationSampleCount() const override {
        return m_control->draws.presentationSampleCount();
    }
    void setTextureFiltering(u32 filtering) override {
        m_control->draws.setTextureFiltering(filtering);
    }
    void setSmoothSprites(bool enabled) override { m_control->draws.setSmoothSprites(enabled); }
    void setAuthoredSilhouettes(bool enabled) override {
        m_control->draws.setAuthoredSilhouettes(enabled);
    }
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                           std::span<const u8> pixels) override {
        m_control->events.emplace_back("create");
        return m_control->draws.createTexture(desc, pixels);
    }
    void updateTexture(Texture& texture, std::span<const u8> pixels) override {
        m_control->events.emplace_back("update");
        m_control->draws.updateTexture(texture, pixels);
    }
    const Texture& whiteTexture() const override { return m_control->draws.whiteTexture(); }
    void draw(const ImmediateBatch& batch, const Texture& texture, const Mat4& transform,
              const DrawState& state) override {
        m_control->events.emplace_back("draw");
        const std::array sources{&texture, state.lightmap, state.maskedTexture, state.nextTexture};
        for (usize index = 0; index < sources.size(); ++index) {
            if (sources[index] != nullptr) {
                m_control->sampled[index] =
                    dynamic_cast<const test::FakeTexture&>(*sources[index]).pixels;
            }
        }
        m_control->draws.draw(batch, texture, transform, state);
    }
    void waitIdle() override { m_control->events.emplace_back("idle"); }
    void detachFromThread() override {
        m_control->detachThread = std::this_thread::get_id();
        ++m_control->detached;
    }
    void attachToThread() override {
        m_control->attachThread = std::this_thread::get_id();
        ++m_control->attached;
    }
    bool applyDepthOfField(const DepthOfField& settings) override {
        m_control->events.emplace_back("dof");
        return m_control->draws.applyDepthOfField(settings);
    }
    bool applyBloom() override {
        m_control->events.emplace_back("bloom");
        return m_control->draws.applyBloom();
    }
    void addHeatSource(const HeatSource& source) override {
        m_control->draws.addHeatSource(source);
    }
    bool applyAmbientOcclusion(const AmbientOcclusion& settings) override {
        m_control->events.emplace_back("ao");
        return m_control->draws.applyAmbientOcclusion(settings);
    }

private:
    std::shared_ptr<Control> m_control;
};

TEST_CASE("a blocked render worker cannot block fixed updates or queue stale frames",
          "[threaded-render][timing]") {
    auto control = std::make_shared<Control>();
    control->blocked = true;
    SECTION("blocked swapchain acquisition") {}
    SECTION("blocked V-Sync presentation") {
        control->blockAtEnd = true;
    }
    ThreadedRenderDevice device(std::make_unique<Backend>(control), {800, 600});
    struct ReleaseOnExit {
        explicit ReleaseOnExit(Control& value) : control(value) {}
        GDL_NON_COPYABLE_NON_MOVABLE(ReleaseOnExit);
        Control& control;
        ~ReleaseOnExit() { control.release(); }
    } const release{*control};
    REQUIRE(device.beginFrame());
    device.endFrame();
    // Always release the worker on assertion failure, so teardown cannot deadlock.
    const bool entered = control->waitForEntry();
    if (!entered) {
        control->release();
    }
    REQUIRE(entered);
    UpdateClock simulation;
    u32 ticks = 0;
    for (u32 step = 0; step < 120; ++step) {
        ticks += simulation.advance(1.0 / 60, 60);
        CHECK_FALSE(device.beginFrame());
    }
    CHECK(ticks == 120);
    control->release();
    device.waitIdle();
    CHECK(control->renderThread != std::this_thread::get_id());
    CHECK(control->draws.frames == 1);
    REQUIRE(device.beginFrame());
    device.endFrame();
    device.waitIdle();
    CHECK(control->draws.frames == 2);
}

TEST_CASE("render packets own uploads geometry auxiliary textures and post-processing order",
          "[threaded-render]") {
    auto control = std::make_shared<Control>();
    {
        ThreadedRenderDevice device(std::make_unique<Backend>(control), {800, 600});
        std::array<u8, 4> pixels{20, 30, 40, 255};
        auto base = device.createTexture({1, 1}, pixels);
        auto lightmap = device.createTexture({1, 1}, pixels);
        auto mask = device.createTexture({1, 1}, pixels);
        auto next = device.createTexture({1, 1}, pixels);
        pixels = {100, 110, 120, 255};
        device.setPresentation(false, 4);
        device.setTextureFiltering(16);
        device.setSmoothSprites(true);
        device.setFramebufferSize({1920, 1080});
        REQUIRE(device.beginFrame());
        device.updateTexture(*base, pixels);
        pixels.fill(0);
        ImmediateBatch batch;
        batch.rect({1, 2, 3, 4}, 0, Color::white());
        DrawState state;
        state.lightmap = lightmap.get();
        state.maskedTexture = mask.get();
        state.nextTexture = next.get();
        device.draw(batch, *base, Mat4{1}, state);
        CHECK_THROWS_AS(device.updateTexture(*base, pixels), std::logic_error);
        batch.clear();
        base.reset();
        lightmap.reset();
        mask.reset();
        next.reset();
        device.applyAmbientOcclusion({});
        device.addHeatSource({Vec2{0.5f}, Vec2{0.1f}, 0.9f, 1});
        device.applyBloom();
        device.applyDepthOfField({});
        device.endFrame();
        device.waitIdle();
        CHECK(device.presentationSampleCount() == 4);
        CHECK(control->extent == Extent2D{1920, 1080});
        CHECK_FALSE(control->draws.presentationVsync);
        CHECK(control->draws.textureFiltering == 16);
        CHECK(control->draws.smoothSprites);
        REQUIRE(control->draws.draws.size() == 1);
        CHECK(control->draws.draws[0].vertices.size() == 6);
        CHECK(control->sampled[0] == std::vector<u8>{100, 110, 120, 255});
        for (usize index = 1; index < 4; ++index) {
            CHECK(control->sampled[index] == std::vector<u8>{20, 30, 40, 255});
        }
        const std::vector<std::string> expected{"create", "create", "create", "create",
                                                "begin",  "update", "draw",   "ao",
                                                "bloom",  "dof",    "end"};
        REQUIRE(control->events.size() >= expected.size());
        CHECK(std::equal(expected.begin(), expected.end(), control->events.begin()));
        CHECK(control->events[expected.size()] == "idle"); // fence before texture retirement
    }
    CHECK(control->destroyed);
    CHECK(control->destroyThread == control->renderThread);
}

TEST_CASE("streamed texture updates survive a skipped swapchain frame", "[threaded-render]") {
    auto control = std::make_shared<Control>();
    control->skip = true;
    ThreadedRenderDevice device(std::make_unique<Backend>(control), {800, 600});
    const std::array<u8, 4> original{0, 0, 0, 255};
    const std::array<u8, 4> latest{100, 50, 25, 255};
    auto texture = device.createTexture({1, 1}, original);
    REQUIRE(device.beginFrame());
    device.updateTexture(*texture, latest);
    device.endFrame();
    device.waitIdle();
    CHECK(control->draws.textureUpdates == 0);
    control->skip = false; // worker is idle behind the completed barrier
    REQUIRE(device.beginFrame());
    ImmediateBatch batch;
    batch.rect({0, 0, 4, 4}, 0, Color::white());
    device.draw(batch, *texture, Mat4{1});
    device.endFrame();
    device.waitIdle();
    CHECK(control->draws.textureUpdates == 1);
    CHECK(control->sampled[0] == std::vector<u8>(latest.begin(), latest.end()));
}

TEST_CASE("the backend is handed from the application thread to the worker that drives it",
          "[threaded-render][renderer]") {
    auto control = std::make_shared<Control>();
    {
        ThreadedRenderDevice device(std::make_unique<Backend>(control), {800, 600});
        REQUIRE(device.beginFrame());
        device.endFrame();
        device.waitIdle();
    }
    CHECK(control->detached == 1);
    CHECK(control->attached == 1);
    CHECK(control->detachThread == std::this_thread::get_id());
    CHECK(control->attachThread != std::this_thread::get_id());
    CHECK(control->attachThread == control->renderThread);
    CHECK(control->destroyThread == control->attachThread);
}

TEST_CASE("render worker failures reach the application without hanging a barrier",
          "[threaded-render]") {
    auto control = std::make_shared<Control>();
    control->fail = true;
    ThreadedRenderDevice device(std::make_unique<Backend>(control), {800, 600});
    REQUIRE(device.beginFrame());
    device.endFrame();
    CHECK_THROWS_WITH(device.waitIdle(), "test presentation failure");
    CHECK_THROWS_WITH(device.beginFrame(), "test presentation failure");
}

TEST_CASE("recording validates frame and texture ownership and skips minimized windows",
          "[threaded-render]") {
    auto control = std::make_shared<Control>();
    ThreadedRenderDevice device(std::make_unique<Backend>(control), {0, 0});
    CHECK_FALSE(device.beginFrame());
    device.setFramebufferSize({640, 448});
    REQUIRE(device.beginFrame());
    CHECK_THROWS_AS(device.beginFrame(), std::logic_error);
    CHECK_THROWS_AS(device.waitIdle(), std::logic_error);
    ImmediateBatch batch;
    batch.rect({0, 0, 4, 4}, 0, Color::white());
    const test::FakeTexture foreign(1, 1);
    CHECK_THROWS_AS(device.draw(batch, foreign, Mat4{1}), std::invalid_argument);
    device.draw(batch, device.whiteTexture(), Mat4{1});
    device.endFrame();
    device.waitIdle();
    CHECK_THROWS_AS(device.endFrame(), std::logic_error);
    CHECK_THROWS_AS(device.applyBloom(), std::logic_error);
    CHECK(control->draws.frames == 1);
}

TEST_CASE("render packets capture scoped world fog without leaking it into UI or later frames",
          "[threaded-render][scene-fog]") {
    auto control = std::make_shared<Control>();
    ThreadedRenderDevice device(std::make_unique<Backend>(control), {640, 448});
    ImmediateBatch batch;
    batch.rect({0, 0, 4, 4}, 0, Color::white());
    const SceneFog first{Vec3{1.0f, .75f, .5f}, -12.0f, 1.25f};
    const SceneFog second{Vec3{.25f, .5f, 1.0f}, -4.0f, .8f};
    REQUIRE(device.beginFrame());
    device.setSceneFog(first);
    device.draw(batch, device.whiteTexture(), Mat4{1});
    device.setSceneFog(second);
    device.draw(batch, device.whiteTexture(), Mat4{1});
    device.setSceneFog({});
    device.draw(batch, device.whiteTexture(), Mat4{1}); // UI after the world scope.
    device.endFrame();
    device.waitIdle();
    REQUIRE(control->draws.draws.size() == 3);
    CHECK(control->draws.draws[0].state.fog == first);
    CHECK(control->draws.draws[1].state.fog == second);
    CHECK(control->draws.draws[2].state.fog == SceneFog{});
    CHECK(device.sceneFog() == SceneFog{});

    REQUIRE(device.beginFrame());
    device.draw(batch, device.whiteTexture(), Mat4{1});
    auto malformed = first;
    malformed.reciprocalScale = std::numeric_limits<f32>::quiet_NaN();
    device.setSceneFog(malformed);
    device.draw(batch, device.whiteTexture(), Mat4{1});
    malformed = second;
    malformed.color.y = std::numeric_limits<f32>::infinity();
    device.setSceneFog(malformed);
    device.draw(batch, device.whiteTexture(), Mat4{1});
    device.endFrame();
    device.waitIdle();
    REQUIRE(control->draws.draws.size() == 6);
    for (usize index = 3; index < 6; ++index) {
        CHECK(control->draws.draws[index].state.fog == SceneFog{});
    }
    CHECK(device.sceneFog() == SceneFog{});
}
} // namespace
