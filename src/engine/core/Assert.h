#pragma once

#include <string_view>

#include "engine/core/Types.h"

namespace gdl::detail {

/** Logs the failure and terminates the process. */
[[noreturn]] void fatal(std::string_view expression, std::string_view file, s32 line,
                        std::string_view message);

} // namespace gdl::detail

/** Terminates with a message. */
#define GDL_FATAL(message) ::gdl::detail::fatal("", __FILE__, __LINE__, (message))

/** Always-on assertion for invariants and unrecoverable setup failures. */
#define GDL_VERIFY(condition, message)                                                             \
    ((condition) ? static_cast<void>(0)                                                            \
                 : ::gdl::detail::fatal(#condition, __FILE__, __LINE__, (message)))

/** Debug-only assertion; compiled out of non-Debug builds. */
#if GDL_DEBUG
#define GDL_ASSERT(condition, message) GDL_VERIFY(condition, message)
#else
#define GDL_ASSERT(condition, message) static_cast<void>(0)
#endif
