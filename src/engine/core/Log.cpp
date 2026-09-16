#include "engine/core/Log.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#ifdef _WIN32
// NOLINTNEXTLINE(readability-identifier-naming): Win32 API name
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char* string);
#endif

namespace gdl::log {

namespace {

const char* levelTag(Level level) {
    switch (level) {
    case Level::Trace: return "trace";
    case Level::Info: return "info ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?????";
}

const auto kStartTime = std::chrono::steady_clock::now();

std::mutex& outputMutex() {
    static std::mutex mutex;
    return mutex;
}

} // namespace

void write(Level level, std::string_view message) {
    const auto elapsed =
        std::chrono::duration<f64>(std::chrono::steady_clock::now() - kStartTime).count();
    const std::string line = std::format("[{:9.3f}] {} {}\n", elapsed, levelTag(level), message);

    const std::lock_guard lock(outputMutex());
    std::FILE* stream = level >= Level::Warn ? stderr : stdout;
    std::fputs(line.c_str(), stream);
    std::fflush(stream);
#ifdef _WIN32
    OutputDebugStringA(line.c_str());
#endif
}

} // namespace gdl::log
