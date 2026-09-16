#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

namespace {

using namespace gdl;

TEST_CASE("fixed-width aliases have the expected sizes", "[core][types]") {
    STATIC_REQUIRE(sizeof(u8) == 1);
    STATIC_REQUIRE(sizeof(u16) == 2);
    STATIC_REQUIRE(sizeof(u32) == 4);
    STATIC_REQUIRE(sizeof(u64) == 8);
    STATIC_REQUIRE(sizeof(s8) == 1);
    STATIC_REQUIRE(sizeof(s16) == 2);
    STATIC_REQUIRE(sizeof(s32) == 4);
    STATIC_REQUIRE(sizeof(s64) == 8);
    STATIC_REQUIRE(sizeof(f32) == 4);
    STATIC_REQUIRE(sizeof(f64) == 8);
    STATIC_REQUIRE(sizeof(usize) == sizeof(void*));
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
