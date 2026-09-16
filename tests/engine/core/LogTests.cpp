#include <catch2/catch_test_macros.hpp>

#include "engine/core/Log.h"

namespace {

using namespace gdl;

TEST_CASE("every log level formats and writes without throwing", "[core][log]") {
    REQUIRE_NOTHROW(log::trace("trace {}", 1));
    REQUIRE_NOTHROW(log::info("info {} {}", "two", 2.5));
    REQUIRE_NOTHROW(log::warn("warn {:>4}", "x"));
    REQUIRE_NOTHROW(log::error("error {}", true));
    REQUIRE_NOTHROW(log::write(log::Level::Info, ""));
}

TEST_CASE("log levels are ordered by severity", "[core][log]") {
    STATIC_REQUIRE(log::Level::Trace < log::Level::Info);
    STATIC_REQUIRE(log::Level::Info < log::Level::Warn);
    STATIC_REQUIRE(log::Level::Warn < log::Level::Error);
}

} // namespace
