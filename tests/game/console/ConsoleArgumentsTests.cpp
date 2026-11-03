#include <cmath>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/console/ConsoleArguments.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("a console word changes case letter by letter", "[dev-console][commands]") {
    CHECK(lowerArgument("GoTo E2_x#3") == "goto e2_x#3");
    CHECK(upperArgument("GoTo e2_x#3") == "GOTO E2_X#3");
    CHECK(lowerArgument("").empty());
    CHECK(upperArgument("").empty());
}

TEST_CASE("a console word reads as an integer only whole", "[dev-console][commands]") {
    CHECK(integerArgument("42") == 42);
    CHECK(integerArgument("-5") == -5);
    CHECK(integerArgument("0") == 0);
    CHECK_FALSE(integerArgument("").has_value());
    CHECK_FALSE(integerArgument("4x").has_value());
    CHECK_FALSE(integerArgument("x4").has_value());
    CHECK_FALSE(integerArgument("+4").has_value());
    CHECK_FALSE(integerArgument("2.5").has_value());
    CHECK_FALSE(integerArgument("2147483648").has_value());
}

TEST_CASE("a console word reads as a number only whole", "[dev-console][commands]") {
    CHECK(numberArgument("2.5") == 2.5f);
    CHECK(numberArgument("-3") == -3.0f);
    CHECK(numberArgument("1e3") == 1000.0f);
    CHECK_FALSE(numberArgument("").has_value());
    CHECK_FALSE(numberArgument("2.5x").has_value());
    CHECK_FALSE(numberArgument("+1").has_value());
    const std::optional<f32> notANumber = numberArgument("nan");
    REQUIRE(notANumber.has_value());
    CHECK(std::isnan(*notANumber));
    const std::optional<f32> infinite = numberArgument("inf");
    REQUIRE(infinite.has_value());
    CHECK(std::isinf(*infinite));
}

} // namespace
