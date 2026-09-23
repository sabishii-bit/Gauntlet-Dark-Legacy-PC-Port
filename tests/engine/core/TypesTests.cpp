#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/SpecialMembers.h"

namespace {

TEST_CASE("standard numeric representations preserve asset and GPU widths", "[core][types]") {
    STATIC_REQUIRE(sizeof(std::uint8_t) == 1);
    STATIC_REQUIRE(sizeof(std::uint16_t) == 2);
    STATIC_REQUIRE(sizeof(std::uint32_t) == 4);
    STATIC_REQUIRE(sizeof(std::uint64_t) == 8);
    STATIC_REQUIRE(sizeof(std::int8_t) == 1);
    STATIC_REQUIRE(sizeof(std::int16_t) == 2);
    STATIC_REQUIRE(sizeof(std::int32_t) == 4);
    STATIC_REQUIRE(sizeof(std::int64_t) == 8);
    STATIC_REQUIRE(sizeof(float) == 4);
    STATIC_REQUIRE(sizeof(double) == 8);
    STATIC_REQUIRE(sizeof(std::size_t) == sizeof(void*));
}

struct Pinned {
    Pinned() = default;
    ~Pinned() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(Pinned);
};

struct Uncopyable {
    Uncopyable() = default;
    ~Uncopyable() = default;
    GDL_NON_COPYABLE(Uncopyable);
};

TEST_CASE("ownership macros delete the intended special members", "[core][types]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<Pinned>);
    STATIC_REQUIRE(!std::is_move_constructible_v<Pinned>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<Uncopyable>);
    STATIC_REQUIRE(!std::is_copy_assignable_v<Uncopyable>);
}

} // namespace
