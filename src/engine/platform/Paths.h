#pragma once

#include <filesystem>

namespace gdl::paths {

/** Directory containing the running executable. */
std::filesystem::path executableDirectory();

} // namespace gdl::paths
