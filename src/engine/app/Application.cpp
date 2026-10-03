#include "engine/app/Application.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/platform/DisplayTiming.h"
#include "engine/platform/Paths.h"

namespace gdl {
namespace {
bool frameTimingEnabled() {
    const char* value = std::getenv("GDL_FRAME_TIMING");
    return value != nullptr && std::string_view(value) == "1";
}

void reportFrameTiming(const FrameTimingTotals& totals, u32 frameRate) {
    log::info("Frame timing: frames={} cap={} fps={:.2f} updates/frame={:.3f} overruns={}",
              totals.frames, frameRate, totals.framesPerSecond(),
              static_cast<f64>(totals.updates) / totals.frames, totals.overruns);
    using Phase = FrameTimingPhase;
    log::info("Frame timing ms avg/max: poll={:.3f}/{:.3f} update={:.3f}/{:.3f} "
              "acquire={:.3f}/{:.3f} render={:.3f}/{:.3f} present={:.3f}/{:.3f} "
              "refresh={:.3f}/{:.3f} wait={:.3f}/{:.3f} oversleep={:.3f}/{:.3f}",
              totals.average(Phase::Poll), totals.maximum(Phase::Poll),
              totals.average(Phase::Update), totals.maximum(Phase::Update),
              totals.average(Phase::Acquire), totals.maximum(Phase::Acquire),
              totals.average(Phase::Render), totals.maximum(Phase::Render),
              totals.average(Phase::Present), totals.maximum(Phase::Present),
              totals.average(Phase::Refresh), totals.maximum(Phase::Refresh),
              totals.average(Phase::Wait), totals.maximum(Phase::Wait),
              totals.average(Phase::Oversleep), totals.maximum(Phase::Oversleep));
}
} // namespace

Application::Application(ApplicationDesc desc) : m_desc(std::move(desc)) {}

Application::~Application() = default;

s32 Application::run() {
    try {
        log::info("Starting {}", m_desc.window.title);
        m_window = createGlfwWindow(m_desc.window);

        RenderDeviceDesc deviceDesc;
        deviceDesc.vsync = m_desc.vsync;
        deviceDesc.sampleCount = m_desc.sampleCount;
        deviceDesc.enableValidation = m_desc.enableValidation;
        deviceDesc.shaderDirectory = paths::executableDirectory() / "shaders";
        m_device = createVulkanRenderDevice(*m_window, deviceDesc);

        checkAssetDirectory();
        onInit();
        u32 lastFrameRate = 0;
        FramePacer framePacer;
        const bool measureFrames = frameTimingEnabled();
        FrameTimingTotals timingTotals;

        while (!m_window->shouldClose() && !m_quitRequested) {
            const auto frameStart = std::chrono::steady_clock::now();
            auto mark = frameStart;
            FrameTimingSample timing;
            const auto measure = [&](FrameTimingPhase phase) {
                if (measureFrames) {
                    const auto now = std::chrono::steady_clock::now();
                    timing.measure(phase, mark, now);
                    mark = now;
                }
            };
            m_window->pollEvents();
            m_clock.tick();
            measure(FrameTimingPhase::Poll);
            if (m_desc.updateRate == 0) {
                onUpdate(m_clock.deltaSeconds());
                ++timing.updates;
            } else {
                m_updateInput.accumulate(m_window->input());
                const u32 ticks = m_updateClock.advance(m_clock.deltaSeconds(), m_desc.updateRate);
                for (u32 tick = 0; tick < ticks && !m_quitRequested; ++tick) {
                    onUpdate(1.0 / m_desc.updateRate);
                    m_updateInput.beginPoll();
                    ++timing.updates;
                }
            }
            measure(FrameTimingPhase::Update);

            const bool drawable = m_device->beginFrame();
            measure(FrameTimingPhase::Acquire);
            if (drawable) {
                onRender(*m_device);
                measure(FrameTimingPhase::Render);
                m_device->endFrame();
            } else {
                m_window->waitWhileMinimized();
            }
            measure(FrameTimingPhase::Present);

            const u32 refreshRate = m_window->refreshRate();
            measure(FrameTimingPhase::Refresh);
            const u32 frameRate = displayFrameRate(m_desc.maxFrameRate, refreshRate);
            if (frameRate != lastFrameRate) {
                log::info("Presentation cap: {} fps on a {} Hz display", frameRate, refreshRate);
                lastFrameRate = frameRate;
            }
            const auto frameFinished = std::chrono::steady_clock::now();
            const auto deadline = framePacer.deadline(frameStart, frameFinished, frameRate);
            std::this_thread::sleep_until(deadline);
            if (measureFrames) {
                const auto wake = std::chrono::steady_clock::now();
                timing.measure(FrameTimingPhase::Wait, frameFinished, wake);
                timing.measure(FrameTimingPhase::Oversleep, deadline, wake);
                timing.elapsedMilliseconds =
                    std::chrono::duration<f64, std::milli>(wake - frameStart).count();
                timing.overran = deadline <= frameFinished;
                timingTotals.add(timing);
                if (timingTotals.frames == 120) {
                    reportFrameTiming(timingTotals, frameRate);
                    timingTotals = {};
                }
            }

            if (m_desc.maxFrames != 0 && m_clock.frameIndex() >= m_desc.maxFrames) {
                log::info("Reached the requested frame limit ({} frames)", m_desc.maxFrames);
                requestQuit();
            }
        }

        m_device->waitIdle();
        onShutdown();
        m_device.reset();
        m_window.reset();
        log::info("Clean shutdown");
        return 0;
    } catch (const std::exception& e) {
        log::error("Unhandled exception: {}", e.what());
        return 1;
    }
}

void Application::checkAssetDirectory() const {
    const auto& dir = m_desc.assetDirectory;
    if (dir.empty()) {
        log::warn("No asset directory configured (pass --assets <dir>)");
        return;
    }
    const auto marker = dir / "STATIC" / "objects.ngc";
    std::error_code ec;
    if (std::filesystem::exists(marker, ec)) {
        log::info("Asset directory: {}", dir.string());
    } else {
        log::warn("Asset directory {} does not contain STATIC/objects.ngc", dir.string());
    }
}

} // namespace gdl
