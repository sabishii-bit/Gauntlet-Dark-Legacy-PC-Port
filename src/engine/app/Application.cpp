#include "engine/app/Application.h"

#include <chrono>
#include <exception>
#include <system_error>
#include <thread>
#include <utility>

#include "engine/core/Log.h"
#include "engine/platform/Paths.h"

namespace gdl {

Application::Application(ApplicationDesc desc) : m_desc(std::move(desc)) {}

Application::~Application() = default;

int Application::run() {
    try {
        log::info("Starting {}", m_desc.window.title);
        m_window = createGlfwWindow(m_desc.window);

        RenderDeviceDesc deviceDesc;
        deviceDesc.vsync = m_desc.vsync;
        deviceDesc.enableValidation = m_desc.enableValidation;
        deviceDesc.shaderDirectory = paths::executableDirectory() / "shaders";
        m_device = createVulkanRenderDevice(*m_window, deviceDesc);

        checkAssetDirectory();
        onInit();

        while (!m_window->shouldClose() && !m_quitRequested) {
            const auto frameStart = std::chrono::steady_clock::now();
            m_window->pollEvents();
            m_clock.tick();
            onUpdate(m_clock.deltaSeconds());

            if (m_device->beginFrame()) {
                onRender(*m_device);
                m_device->endFrame();
            } else {
                m_window->waitWhileMinimized();
            }

            if (m_desc.maxFrameRate != 0) {
                const auto frameTime = std::chrono::duration<f64>(1.0 / m_desc.maxFrameRate);
                std::this_thread::sleep_until(
                    frameStart +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(frameTime));
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
