#pragma once

#include <filesystem>
#include <optional>

namespace gdl::game {

/** Finds an exact native LEVL -> AUDS -> STR reference; no inferred realm music. */
std::optional<std::filesystem::path>
dreamcastLegendsMusic(const std::filesystem::path& levelDirectory);

} // namespace gdl::game
