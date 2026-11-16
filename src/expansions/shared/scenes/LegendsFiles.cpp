#include "expansions/shared/scenes/LegendsFiles.h"

#include <filesystem>
#include <string_view>
#include <system_error>

#include "engine/io/AssetLocator.h"

namespace gdl::game {

bool nonemptyFile(const AssetLocator& files, std::string_view name) {
    const auto path = files.find(name);
    std::error_code error;
    if (!path || !std::filesystem::is_regular_file(*path, error)) {
        return false;
    }
    const auto size = std::filesystem::file_size(*path, error);
    return !error && size != 0;
}

} // namespace gdl::game
