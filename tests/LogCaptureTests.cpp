#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Log.h"

#include "LogCapture.h"

namespace {
using namespace gdl;

TEST_CASE("a logged call's warnings are read back without their time stamps",
          "[test-support][log-capture]") {
    const test::LoggedRun logged = test::logWhile([] {
        log::warn("first {}", 1);
        log::info("not a warning");
        log::error("nor an error");
        log::warn("second");
    });
    CHECK(logged.warnings == std::vector<std::string>{"first 1", "second"});
    CHECK(logged.failure == nullptr);
    // Each capture starts empty.
    CHECK(test::logWhile([] {}).warnings.empty());
    CHECK(test::warningsWhile([] { log::warn("third"); }) == std::vector<std::string>{"third"});
}

TEST_CASE("what a logged call throws is kept, or thrown on with its warnings read",
          "[test-support][log-capture]") {
    const test::LoggedRun logged = test::logWhile([] {
        log::warn("before");
        throw std::runtime_error("broken");
    });
    CHECK(logged.warnings == std::vector<std::string>{"before"});
    REQUIRE(logged.failure != nullptr);
    CHECK_THROWS_AS(std::rethrow_exception(logged.failure), std::runtime_error);
    CHECK_THROWS_AS(test::warningsWhile([] { throw std::runtime_error("broken"); }),
                    std::runtime_error);
}
} // namespace
