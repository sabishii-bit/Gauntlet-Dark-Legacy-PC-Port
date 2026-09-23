#pragma once

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
