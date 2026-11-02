#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "engine/core/Clock.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/platform/DisplayTiming.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/render/RenderDevice.h"

namespace gdl {

struct ApplicationDesc {
    WindowDesc window;
    std::filesystem::path assetDirectory;
    /** A file, relative to the asset directory, that only the program's data holds: run
     * warns when the directory lacks it. Empty checks for nothing. */
    std::filesystem::path assetMarker;
    bool vsync = true;
    u32 sampleCount = 1; ///< requested raster samples; the renderer checks GPU support
    u32 textureFiltering = 8;
    bool enableValidation = false;
    /** The graphics API asked for; when it cannot start on this machine the other stands
     * in, and activeRenderer says which one draws. */
    GraphicsApi renderer = GraphicsApi::Vulkan;
    u64 maxFrames = 0;    ///< quit after this many frames; 0 runs until closed
    u32 maxFrameRate = 0; ///< frames per second, capped by the monitor; 0 uses monitor refresh
    u32 updateRate = 0;   ///< whole simulation ticks per second; 0 updates on every render
    bool threadedPresentation = false; ///< keep online ticks independent of GPU/present waits
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
    f64 renderDeltaSeconds() const { return m_renderDelta; }
    u64 renderedFrames() const { return m_renderedFrames; }
    f32 presentationAlpha() const {
        return m_desc.updateRate == 0 ? 1.0f : m_updateClock.fraction(m_desc.updateRate);
    }
    const std::filesystem::path& assetDirectory() const { return m_desc.assetDirectory; }
    /** The API drawing the frames, once run has brought the window up. */
    GraphicsApi activeRenderer() const { return m_renderer; }

    void requestQuit() { m_quitRequested = true; }
    /** Caps frames from the next frame on, never above the monitor; 0 uses monitor refresh. */
    void setMaxFrameRate(u32 rate) { m_desc.maxFrameRate = rate; }
    u32 maxFrameRate() const { return m_desc.maxFrameRate; }
    /** Changes the simulation clock without changing presentation or saved settings.
     * A request during onUpdate ends the old-rate batch after that callback. */
    void setUpdateRate(u32 rate);
    u32 updateRate() const { return m_desc.updateRate; }

private:
    void checkAssetDirectory() const;
    /** Brings up a window made for `api` and its device; false, with the reason logged,
     * when that API cannot start here. */
    bool startRenderer(GraphicsApi api);

    ApplicationDesc m_desc;
    GraphicsApi m_renderer = GraphicsApi::Vulkan;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<RenderDevice> m_device;
    FrameClock m_clock;
    UpdateClock m_updateClock;
    Input m_updateInput;
    f64 m_renderDelta = 0;
    u64 m_renderedFrames = 0;
    bool m_quitRequested = false;
};

} // namespace gdl
