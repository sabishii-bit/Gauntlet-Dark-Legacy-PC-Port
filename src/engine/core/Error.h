#pragma once

#include <stdexcept>

namespace gdl {

/** A file could not be found, opened or read. */
class FileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** File contents do not match the format the reader expects. */
class FormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace gdl
