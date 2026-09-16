#pragma once

#include <cstddef>
#include <cstdint>

namespace gdl {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;
using usize = std::size_t;

} // namespace gdl

// NOLINTBEGIN(bugprone-macro-parentheses): the argument is a type name
/** Deletes copy construction and copy assignment. */
#define GDL_NON_COPYABLE(Type)                                                                     \
    Type(const Type&) = delete;                                                                    \
    Type& operator=(const Type&) = delete

/** Deletes copy and move construction and assignment. */
#define GDL_NON_COPYABLE_NON_MOVABLE(Type)                                                         \
    GDL_NON_COPYABLE(Type);                                                                        \
    Type(Type&&) = delete;                                                                         \
    Type& operator=(Type&&) = delete
// NOLINTEND(bugprone-macro-parentheses)
