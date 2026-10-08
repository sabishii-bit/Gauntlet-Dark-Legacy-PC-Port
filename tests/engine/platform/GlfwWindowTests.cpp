#ifdef _WIN32
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

#include "engine/platform/Window.h"

TEST_CASE("controller polling leaves an idle GLFW window responsive to Windows",
          "[gpu][window-responsive]") {
    gdl::WindowDesc desc;
    desc.title = "gdl background event test";
    desc.width = 320;
    desc.height = 240;
    auto window = gdl::createGlfwWindow(desc);
    const HWND handle = FindWindowW(nullptr, L"gdl background event test");
    REQUIRE(handle != nullptr);
    DWORD process = 0;
    REQUIRE(GetWindowThreadProcessId(handle, &process) == GetCurrentThreadId());
    REQUIRE(process == GetCurrentProcessId());
    // Run this filter in a fresh process: other window tests can initialize SDL video
    // and mask the regression. Windows' idle hung timeout is currently five seconds.
    // No renderer, input injection or keepalive messages should reset that timeout.
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    bool hung = false;
    while (std::chrono::steady_clock::now() < end) {
        window->pollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        hung = hung || IsHungAppWindow(handle) != FALSE;
    }
    CHECK_FALSE(hung);
}
#endif
