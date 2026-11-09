#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"

namespace {

namespace fs = std::filesystem;
using std::chrono::hours;

/** Leaves `dir` holding a file, last written `age` before `now`. */
void leaveFolder(const fs::path& dir, fs::file_time_type now, hours age) {
    fs::create_directories(dir / "inner");
    std::ofstream(dir / "inner" / "fixture.bin") << "left behind";
    fs::last_write_time(dir, now - age);
}

TEST_CASE("each test process keeps its scratch directories under a root named for it",
          "[test-support]") {
    const fs::path& root = gdl::test::scratchRoot();
    CHECK(root.filename() == "p" + std::to_string(gdl::test::processId()));
    CHECK(root.parent_path().filename() == "gdl-tests");
    CHECK(fs::equivalent(root.parent_path().parent_path(), fs::temp_directory_path()));
    CHECK(fs::is_directory(root));
    CHECK(gdl::test::scratchDirectory("scratch-root").parent_path() == root);
}

TEST_CASE("a scratch directory asked for again by name comes back fresh and empty",
          "[test-support]") {
    const fs::path first = gdl::test::scratchDirectory("scratch-fresh");
    fs::create_directories(first / "inner");
    std::ofstream(first / "inner" / "fixture.bin") << "stale";
    const fs::path second = gdl::test::scratchDirectory("scratch-fresh");
    CHECK(second == first);
    CHECK(fs::is_directory(second));
    CHECK(fs::is_empty(second));
}

TEST_CASE("the scratch sweep removes day-old leftovers but no recent or own root",
          "[test-support]") {
    const fs::path folder = gdl::test::scratchDirectory("scratch-sweep");
    const auto now = fs::file_time_type::clock::now();
    leaveFolder(folder / "p101", now, hours{25});       // a run that never exited
    leaveFolder(folder / "fixed-name", now, hours{72}); // a folder from before per-process roots
    leaveFolder(folder / "p102", now, hours{23});       // a run within its day
    leaveFolder(folder / "p103", now, hours{48});       // the sweeping process's own root
    gdl::test::sweepStaleScratch(folder, now, folder / "p103");
    CHECK_FALSE(fs::exists(folder / "p101"));
    CHECK_FALSE(fs::exists(folder / "fixed-name"));
    CHECK(fs::exists(folder / "p102" / "inner" / "fixture.bin"));
    CHECK(fs::exists(folder / "p103" / "inner" / "fixture.bin"));
}

} // namespace
