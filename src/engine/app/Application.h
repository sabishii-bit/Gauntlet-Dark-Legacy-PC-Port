#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "engine/core/Clock.h"
#include "engine/core/SpecialMembers.h"
#include "engine/platform/Input.h"
#include "engine/platform/Window.h"
#include "engine/render/RenderDevice.h"

namespace gdl {

struct ApplicationDesc {
    WindowDesc window;
    std::filesystem::path assetDirectory;
    bool vsync = true;
    bool enableValidation = false;
    std::uint64_t maxFrames = 0; ///< quit after this many frames; 0 runs until closed
    unsigned int maxFrameRate =
        0; ///< sleep to hold this many frames per second; 0 leaves it to vsync
};

/** Owns the window, the render device and the frame loop. Subclass and override the hooks. */
class Application {
public:
    explicit Application(ApplicationDesc desc);
    virtual ~Application();

    GDL_NON_COPYABLE_NON_MOVABLE(Application);

    /** Runs until the window closes or requestQuit() is called. Returns the exit code. */
    int run();

protected:
    virtual void onInit() {}
    virtual void onUpdate(double /*deltaSeconds*/) {}
    virtual void onRender(RenderDevice& /*device*/) {}
    virtual void onShutdown() {}

    Window& window() { return *m_window; }
    RenderDevice& renderDevice() { return *m_device; }
    const Input& input() const { return m_window->input(); }
    const FrameClock& clock() const { return m_clock; }
    const std::filesystem::path& assetDirectory() const { return m_desc.assetDirectory; }

    void requestQuit() { m_quitRequested = true; }
    /** Holds this many frames a second from the next frame on; 0 leaves it to vsync. */
    void setMaxFrameRate(unsigned int rate) { m_desc.maxFrameRate = rate; }
    unsigned int maxFrameRate() const { return m_desc.maxFrameRate; }

private:
    void checkAssetDirectory() const;

    ApplicationDesc m_desc;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<RenderDevice> m_device;
    FrameClock m_clock;
    bool m_quitRequested = false;
};

} // namespace gdl
