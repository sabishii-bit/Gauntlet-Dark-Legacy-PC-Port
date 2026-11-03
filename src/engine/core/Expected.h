#pragma once

#include <utility>
#include <variant>

#include "engine/core/Assert.h"
#include "engine/core/Types.h"

namespace gdl {

/** An error handed back where a value was wanted: what an Expected is made from when there is
 * no value to give. */
template <class E> class Unexpected {
public:
    explicit Unexpected(E error) : m_error(std::move(error)) {}

    const E& error() const& { return m_error; }
    E&& error() && { return std::move(m_error); }

private:
    E m_error;
};

template <class E> Unexpected(E) -> Unexpected<E>;

/**
 * What a function gives back: its value, or the error that kept it from one, never both.
 * Reading the one it does not hold is a programmer error.
 */
template <class T, class E> class Expected {
public:
    /** Holds `value`. */
    Expected(T value) : m_held(std::in_place_index<kValue>, std::move(value)) {}
    /** Holds the error `unexpected` carries. */
    Expected(Unexpected<E> unexpected)
        : m_held(std::in_place_index<kError>, std::move(unexpected).error()) {}

    // NOLINTBEGIN(readability-identifier-naming): the names std::expected exposes
    bool has_value() const noexcept { return m_held.index() == kValue; }
    /** The value, or `fallback` made into one when there is none. */
    template <class U> T value_or(U&& fallback) const& {
        return has_value() ? std::get<kValue>(m_held) : static_cast<T>(std::forward<U>(fallback));
    }
    // NOLINTEND(readability-identifier-naming)
    explicit operator bool() const noexcept { return has_value(); }

    const T& value() const& {
        GDL_VERIFY(has_value(), "Expected::value read where an error is held");
        return std::get<kValue>(m_held);
    }
    T& value() & {
        GDL_VERIFY(has_value(), "Expected::value read where an error is held");
        return std::get<kValue>(m_held);
    }
    const E& error() const& {
        GDL_VERIFY(!has_value(), "Expected::error read where a value is held");
        return std::get<kError>(m_held);
    }
    E& error() & {
        GDL_VERIFY(!has_value(), "Expected::error read where a value is held");
        return std::get<kError>(m_held);
    }

    const T& operator*() const& { return value(); }
    T& operator*() & { return value(); }
    const T* operator->() const { return &value(); }
    T* operator->() { return &value(); }

private:
    static constexpr usize kValue = 0;
    static constexpr usize kError = 1;

    std::variant<T, E> m_held;
};

} // namespace gdl
