#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {
std::vector<std::string> loadLegendsN64Scroll(const std::filesystem::path& directory,
                                              std::span<const u8> program, s32 number);
}
