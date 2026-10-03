#pragma once

#include <filesystem>
#include <memory>

#include "engine/core/Clock.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/render/RenderDevice.h"

namespace gdl {

struct ApplicationDesc {
    WindowDesc window;
    std::filesystem::path assetDirectory;
    bool vsync = true;
    u32 sampleCount = 1; ///< requested raster samples; the renderer checks GPU support
    bool enableValidation = false;
    u64 maxFrames = 0;    ///< quit after this many frames; 0 runs until closed
    u32 maxFrameRate = 0; ///< frames per second, capped by the monitor; 0 uses monitor refresh
    u32 updateRate = 0;   ///< whole simulation ticks per second; 0 updates on every render
};

/** Owns the window, the render device and the frame loop. Subclass and override the hooks. */
class Application {
public:
    explicit Application(ApplicationDesc desc);
    virtual ~Application();

    GDL_NON_COPYABLE_NON_MOVABLE(Application);

    /** Runs until the window closes or requestQuit() is called. Returns the exit code. */
    s32 run();

protected:
    virtual void onInit() {}
    virtual void onUpdate(f64 /*deltaSeconds*/) {}
    virtual void onRender(RenderDevice& /*device*/) {}
    virtual void onShutdown() {}

    Window& window() { return *m_window; }
    RenderDevice& renderDevice() { return *m_device; }
    const Input& input() const {
        return m_desc.updateRate == 0 ? m_window->input() : m_updateInput;
    }
    const FrameClock& clock() const { return m_clock; }
    f32 presentationAlpha() const {
        return m_desc.updateRate == 0 ? 1.0f : m_updateClock.fraction(m_desc.updateRate);
    }
    const std::filesystem::path& assetDirectory() const { return m_desc.assetDirectory; }

    void requestQuit() { m_quitRequested = true; }
    /** Caps frames from the next frame on, never above the monitor; 0 uses monitor refresh. */
    void setMaxFrameRate(u32 rate) { m_desc.maxFrameRate = rate; }
    u32 maxFrameRate() const { return m_desc.maxFrameRate; }

private:
    void checkAssetDirectory() const;

    ApplicationDesc m_desc;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<RenderDevice> m_device;
    FrameClock m_clock;
    UpdateClock m_updateClock;
    Input m_updateInput;
    bool m_quitRequested = false;
};

} // namespace gdl
