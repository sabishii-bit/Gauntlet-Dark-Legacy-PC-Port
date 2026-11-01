#pragma once

#include <optional>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl {

/** Native US Legends level-table routing; null means no authored soundtrack. */
std::optional<u16> legendsN64SongForLevel(std::string_view directoryName);

} // namespace gdl
