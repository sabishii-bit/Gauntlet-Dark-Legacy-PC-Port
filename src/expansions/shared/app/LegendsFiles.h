#pragma once

#include <string_view>

#include "engine/io/AssetLocator.h"

namespace gdl::game {

/** Whether `name` under `files`, found whatever its letter case, is a file that holds something:
 * an extraction cut short leaves its files empty. */
bool nonemptyFile(const AssetLocator& files, std::string_view name);

} // namespace gdl::game
