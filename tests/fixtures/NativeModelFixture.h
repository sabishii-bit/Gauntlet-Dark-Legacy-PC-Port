#pragma once

#include <filesystem>

namespace gdl::test {

/** Writes native model/texture archives from a synthetic OBJ/PNG fixture directory.
 * Geometry uses the console's fixed-point precision and colours use RGB5A3.
 * This is test input construction, never a runtime asset fallback. */
void convertModelFixture(const std::filesystem::path& directory);

/** Converts every synthetic model/texture fixture below root, including root itself. */
void convertModelFixtures(const std::filesystem::path& root);

} // namespace gdl::test
