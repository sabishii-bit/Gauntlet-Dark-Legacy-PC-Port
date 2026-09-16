#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/platform/Paths.h"

namespace {

using namespace gdl;

TEST_CASE("executableDirectory is the directory holding the test binary", "[platform][paths]") {
    const std::filesystem::path dir = paths::executableDirectory();
    REQUIRE(dir.is_absolute());
    REQUIRE(std::filesystem::is_directory(dir));
    const bool hasBinary =
        std::filesystem::exists(dir / "tests.exe") || std::filesystem::exists(dir / "tests");
    REQUIRE(hasBinary);
}

} // namespace
