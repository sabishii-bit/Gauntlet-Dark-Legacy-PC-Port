#include "engine/app/Application.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/platform/DisplayTiming.h"
#include "engine/platform/Paths.h"
#include "engine/render/RenderTypes.h"
#include "engine/render/ThreadedRenderDevice.h"

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

bool Application::startRenderer(GraphicsApi api) {
    WindowDesc window = m_desc.window;
    window.api = api;
    RenderDeviceDesc deviceDesc;
    deviceDesc.applicationName = m_desc.name;
    deviceDesc.vsync = m_desc.vsync;
    deviceDesc.sampleCount = m_desc.sampleCount;
    deviceDesc.textureFiltering = m_desc.textureFiltering;
    deviceDesc.enableValidation = m_desc.enableValidation;
    deviceDesc.shaderDirectory = paths::executableDirectory() / "shaders";
    try {
        m_window = createGlfwWindow(window);
        m_device = api == GraphicsApi::OpenGl ? createOpenGlRenderDevice(*m_window, deviceDesc)
                                              : createVulkanRenderDevice(*m_window, deviceDesc);
    } catch (const RenderDeviceUnavailable& e) {
        log::warn("{} cannot start here: {}", graphicsApiName(api), e.what());
        m_device.reset();
        m_window.reset();
        return false;
    }
    m_renderer = api;
    return true;
}

void Application::setUpdateRate(u32 rate) {
    if (rate != m_desc.updateRate) {
        m_desc.updateRate = rate;
        // A fraction accumulated in an offline clock must not become a burst
        // of online updates (or vice versa) when entering/leaving a room.
        m_updateClock = {};
    }
}

s32 Application::run() {
    try {
        log::info("Starting {}", m_desc.window.title);
        // The API asked for, else the other: a machine without Vulkan plays through OpenGL.
        if (!startRenderer(m_desc.renderer)) {
            const GraphicsApi other = otherGraphicsApi(m_desc.renderer);
            log::warn("Trying {} instead", graphicsApiName(other));
            if (!startRenderer(other)) {
                log::error("Neither Vulkan nor OpenGL can draw on this machine");
                return 1;
            }
        }
        if (m_desc.threadedPresentation) {
            m_device = std::make_unique<ThreadedRenderDevice>(std::move(m_device),
                                                              m_window->framebufferSize());
        }

        checkAssetDirectory();
        onInit();
        u32 lastFrameRate = 0;
        FramePacer framePacer;
        const bool measureFrames = frameTimingEnabled();
        FrameTimingTotals timingTotals;
        UpdateClock presentationClock;
        bool renderDue = true;
        f64 renderElapsed = 0;

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
            renderElapsed += m_clock.deltaSeconds();
            measure(FrameTimingPhase::Poll);
            if (m_desc.updateRate == 0) {
                onUpdate(m_clock.deltaSeconds());
                ++timing.updates;
            } else {
                m_updateInput.accumulate(m_window->input());
                const u32 rate = m_desc.updateRate;
                const u32 ticks = m_updateClock.advance(m_clock.deltaSeconds(), rate);
                for (u32 tick = 0; tick < ticks && !m_quitRequested; ++tick) {
                    onUpdate(1.0 / rate);
                    m_updateInput.beginPoll();
                    ++timing.updates;
                    if (m_desc.updateRate != rate) {
                        break; // The remaining ticks belonged to the previous clock.
                    }
                }
            }
            measure(FrameTimingPhase::Update);

            const u32 refreshRate = m_window->refreshRate();
            const u32 frameRate = displayFrameRate(m_desc.maxFrameRate, refreshRate);
            if (m_desc.threadedPresentation) {
                m_device->setFramebufferSize(m_window->framebufferSize());
                renderDue =
                    presentationClock.advance(m_clock.deltaSeconds(), frameRate) > 0 || renderDue;
            }
            const bool drawable =
                (!m_desc.threadedPresentation || renderDue) && m_device->beginFrame();
            measure(FrameTimingPhase::Acquire);
            if (drawable) {
                m_renderDelta = renderElapsed;
                onRender(*m_device);
                measure(FrameTimingPhase::Render);
                m_device->endFrame();
                renderElapsed = 0;
                renderDue = false;
                ++m_renderedFrames;
            } else if (!m_desc.threadedPresentation) {
                m_window->waitWhileMinimized();
            }
            measure(FrameTimingPhase::Present);

            measure(FrameTimingPhase::Refresh);
            if (frameRate != lastFrameRate) {
                log::info("Presentation cap: {} fps on a {} Hz display", frameRate, refreshRate);
                lastFrameRate = frameRate;
            }
            const auto frameFinished = std::chrono::steady_clock::now();
            // Rendering at 30 Hz must not batch 60 Hz online input/network ticks in pairs.
            // A busy or minimized renderer still polls and services the session normally.
            const u32 loopRate =
                m_desc.threadedPresentation ? std::max(frameRate, m_desc.updateRate) : frameRate;
            const auto deadline = framePacer.deadline(frameStart, frameFinished, loopRate);
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

            if (m_desc.maxFrames != 0 && m_renderedFrames >= m_desc.maxFrames) {
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
    const auto& marker = m_desc.assetMarker;
    std::error_code ec;
    if (marker.empty() || std::filesystem::exists(dir / marker, ec)) {
        log::info("Asset directory: {}", paths::utf8(dir));
    } else {
        log::warn("Asset directory {} does not contain {}", paths::utf8(dir),
                  paths::genericUtf8(marker));
    }
}

} // namespace gdl
