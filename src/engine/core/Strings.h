#pragma once

#include <string>
#include <string_view>

namespace gdl {

/** Upper-cases an asset name and drops everything from the first whitespace, as the game does. */
std::string normalizeAssetName(std::string_view name);

std::string toLowerAscii(std::string_view text);

} // namespace gdl
