#include <catch2/catch_test_macros.hpp>

#include "engine/platform/Window.h"

TEST_CASE("window cursors can be installed, replaced and reset without changing input capture",
          "[gpu][cursor-window]") {
    auto window = gdl::createGlfwWindow({"gdl cursor test", 320, 240});
    CHECK_FALSE(window->setCursor({}, 0, 0));
    const auto first = gdl::Image::filled(16, 16, gdl::Color::rgba(200, 0, 160, 255));
    REQUIRE(window->setCursor(first, 2, 3));
    CHECK_FALSE(window->cursorCaptured());
    CHECK_FALSE(window->setCursor(first, 16, 0));
    CHECK_FALSE(window->setCursor(first, 0, 16));
    CHECK_FALSE(window->setCursor(gdl::Image{16, 16, {1, 2, 3}}, 0, 0));
    REQUIRE(window->setCursor(gdl::Image::filled(32, 32, gdl::Color::white()), 5, 5));
    window->pollEvents();
    CHECK_FALSE(window->cursorCaptured());
    window->resetCursor();
    window->resetCursor();
    REQUIRE(window->setCursor(first, 0, 0)); // destruction owns the final live cursor
}

#ifdef _WIN32
#include <chrono>
#include <thread>

#include <windows.h>

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
