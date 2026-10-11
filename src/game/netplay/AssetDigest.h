#pragma once

#include <filesystem>
#include <stop_token>
#include <string>

namespace gdl::game {
/** Canonical native-tree digest shared with the development harness. Throws on ambiguity,
 * links, mutation or cancellation. Run on a worker, never on the frame thread. */
std::string assetDigest(const std::filesystem::path& root, const std::stop_token& stop = {});
} // namespace gdl::game
