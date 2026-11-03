#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Expected.h"
#include "engine/core/Types.h"

namespace {

using gdl::Expected;
using gdl::s32;
using gdl::Unexpected;

/** Halves an even number; an odd one is refused with the reason. */
Expected<s32, std::string> half(s32 number) {
    if (number % 2 != 0) {
        return Unexpected(std::string{"odd"});
    }
    return number / 2;
}

TEST_CASE("an expected value reads through every accessor", "[core][expected]") {
    Expected<std::vector<s32>, std::string> held(std::vector<s32>{1, 2});
    REQUIRE(held.has_value());
    CHECK(static_cast<bool>(held));
    CHECK(held.value() == std::vector<s32>{1, 2});
    CHECK(*held == std::vector<s32>{1, 2});
    CHECK(held->size() == 2);
    held->push_back(3);
    (*held).push_back(4);
    held.value().push_back(5);
    CHECK(held.value() == std::vector<s32>{1, 2, 3, 4, 5});
    const auto& seen = held;
    CHECK(seen->size() == 5);
    CHECK((*seen)[4] == 5);
    CHECK(seen.value_or(std::vector<s32>{}) == std::vector<s32>{1, 2, 3, 4, 5});
}

TEST_CASE("an expected error is the one it was handed, and the fallback stands in for the value",
          "[core][expected]") {
    Expected<s32, std::string> refused = Unexpected(std::string{"no"});
    REQUIRE_FALSE(refused.has_value());
    CHECK_FALSE(static_cast<bool>(refused));
    CHECK(refused.error() == "no");
    refused.error() += " more";
    const auto& seen = refused;
    CHECK(seen.error() == "no more");
    CHECK(seen.value_or(7) == 7);
}

TEST_CASE("a function gives back its value or an error", "[core][expected]") {
    const auto even = half(8);
    REQUIRE(even.has_value());
    CHECK(*even == 4);
    const auto odd = half(7);
    REQUIRE_FALSE(odd.has_value());
    CHECK(odd.error() == "odd");
}

TEST_CASE("a value and an error of the same type stay apart", "[core][expected]") {
    const Expected<std::string, std::string> value(std::string{"text"});
    const Expected<std::string, std::string> error = Unexpected(std::string{"text"});
    CHECK(value.has_value());
    CHECK(*value == "text");
    CHECK_FALSE(error.has_value());
    CHECK(error.error() == "text");
    CHECK(error.value_or("fallback") == "fallback");
}

TEST_CASE("an unexpected takes its error type from what it is given", "[core][expected]") {
    STATIC_REQUIRE(std::is_same_v<decltype(Unexpected(std::string{})), Unexpected<std::string>>);
    STATIC_REQUIRE(std::is_same_v<decltype(Unexpected(s32{3})), Unexpected<s32>>);
    const Unexpected wrapped(std::string{"why"});
    CHECK(wrapped.error() == "why");
}

} // namespace
