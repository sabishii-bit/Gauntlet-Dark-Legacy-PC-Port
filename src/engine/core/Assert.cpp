#include "engine/core/Assert.h"

#include <cstdlib>

#include "engine/core/Log.h"

namespace gdl::detail {

void fatal(std::string_view expression, std::string_view file, int line, std::string_view message) {
    if (expression.empty()) {
        log::error("FATAL: {} ({}:{})", message, file, line);
    } else {
        log::error("ASSERTION FAILED: {} -- {} ({}:{})", expression, message, file, line);
    }
    std::abort();
}

} // namespace gdl::detail
