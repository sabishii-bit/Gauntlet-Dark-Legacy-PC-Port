#include <algorithm>
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
    const std::string name = root.filename().string();
    const std::string prefix = "p" + std::to_string(gdl::test::processId()) + "-";
    REQUIRE(name.starts_with(prefix));
    const std::string start = name.substr(prefix.size());
    CHECK_FALSE(start.empty());
    CHECK(std::ranges::all_of(start, [](char c) { return c >= '0' && c <= '9'; }));
    CHECK(&gdl::test::scratchRoot() == &root);
    CHECK(root.parent_path().filename() == "gdl-tests");
    CHECK(fs::equivalent(root.parent_path().parent_path(), fs::temp_directory_path()));
    CHECK(fs::is_directory(root));
    CHECK(gdl::test::scratchDirectory("scratch-root").parent_path() == root);
}

TEST_CASE("a process given a reused id names its scratch root by its own start", "[test-support]") {
    CHECK(gdl::test::scratchRootName(101, 1000) == "p101-1000");
    CHECK(gdl::test::scratchRootName(101, 1000) != gdl::test::scratchRootName(101, 1001));
    CHECK(gdl::test::scratchRootName(101, 1000) != gdl::test::scratchRootName(10, 11000));
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
    const fs::path left = folder / gdl::test::scratchRootName(101, 1000);
    const fs::path reused = folder / gdl::test::scratchRootName(101, 9000);
    const fs::path recent = folder / gdl::test::scratchRootName(102, 5000);
    const fs::path own = folder / gdl::test::scratchRootName(103, 2000);
    leaveFolder(left, now, hours{25});                  // a run that never exited
    leaveFolder(reused, now, hours{0});                 // a live run given that run's id
    leaveFolder(folder / "p104", now, hours{30});       // a root named before roots had a start
    leaveFolder(folder / "fixed-name", now, hours{72}); // a folder from before per-process roots
    leaveFolder(recent, now, hours{23});                // a run within its day
    leaveFolder(own, now, hours{48});                   // the sweeping process's own root
    gdl::test::sweepStaleScratch(folder, now, own);
    CHECK_FALSE(fs::exists(left));
    CHECK_FALSE(fs::exists(folder / "p104"));
    CHECK_FALSE(fs::exists(folder / "fixed-name"));
    CHECK(fs::exists(reused / "inner" / "fixture.bin"));
    CHECK(fs::exists(recent / "inner" / "fixture.bin"));
    CHECK(fs::exists(own / "inner" / "fixture.bin"));
}

} // namespace
