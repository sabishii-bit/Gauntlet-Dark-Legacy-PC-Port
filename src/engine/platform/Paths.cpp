#include "engine/platform/Paths.h"

#ifdef _WIN32
#include <array>
#include <string_view>

#include <windows.h> // IWYU pragma: keep
#else
#include <system_error>
#endif

namespace gdl::paths {

std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH * 4> buffer{};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
#else
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::filesystem::current_path();
    }
    return exe.parent_path();
#endif
}

} // namespace gdl::paths
