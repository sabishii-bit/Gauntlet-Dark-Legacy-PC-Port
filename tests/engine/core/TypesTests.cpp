#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"

namespace {

TEST_CASE("numeric aliases retain standard type identities", "[core][types]") {
    STATIC_REQUIRE(std::is_same_v<gdl::u8, std::uint8_t>);
    STATIC_REQUIRE(std::is_same_v<gdl::u16, std::uint16_t>);
    STATIC_REQUIRE(std::is_same_v<gdl::u32, std::uint32_t>);
    STATIC_REQUIRE(std::is_same_v<gdl::u64, std::uint64_t>);
    STATIC_REQUIRE(std::is_same_v<gdl::s8, std::int8_t>);
    STATIC_REQUIRE(std::is_same_v<gdl::s16, std::int16_t>);
    STATIC_REQUIRE(std::is_same_v<gdl::s32, std::int32_t>);
    STATIC_REQUIRE(std::is_same_v<gdl::s64, std::int64_t>);
    STATIC_REQUIRE(std::is_same_v<gdl::f32, float>);
    STATIC_REQUIRE(std::is_same_v<gdl::f64, double>);
    STATIC_REQUIRE(std::is_same_v<gdl::usize, std::size_t>);
}

TEST_CASE("ordinary integers interoperate with fixed-width interfaces on supported platforms",
          "[core][types]") {
    // These are the Windows/Linux ABIs supported by the renderer and asset bridge.
    // Check identity, not just size: pointers, spans and template arguments must agree.
    STATIC_REQUIRE(std::is_same_v<int, std::int32_t>);
    STATIC_REQUIRE(std::is_same_v<unsigned int, std::uint32_t>);
    STATIC_REQUIRE(sizeof(int) == 4);
    STATIC_REQUIRE(sizeof(unsigned int) == 4);
}

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
