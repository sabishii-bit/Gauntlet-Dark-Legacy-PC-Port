#pragma once

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace gdl::test {

/** What the log warned of while a call ran, and what the call threw. */
struct LoggedRun {
    std::vector<std::string> warnings; ///< each warning's message, without its time stamp
    std::exception_ptr failure;        ///< null when the call returned
};

namespace error_stream {
/** Closes a stream the capture opened. */
struct FileCloser {
    void operator()(std::FILE* file) const { std::fclose(file); }
};

// The C runtime's calls that point the error stream, where the log warns, at another file.
#if defined(_WIN32)
inline std::FILE* openForWriting(const std::filesystem::path& file) {
    return _wfopen(file.c_str(), L"w");
}
inline int descriptorOf(std::FILE* stream) {
    return _fileno(stream);
}
inline int duplicateDescriptor(int descriptor) {
    return _dup(descriptor);
}
inline int redirectDescriptor(int from, int onto) {
    return _dup2(from, onto);
}
inline int closeDescriptor(int descriptor) {
    return _close(descriptor);
}
#else
inline std::FILE* openForWriting(const std::filesystem::path& file) {
    return std::fopen(file.c_str(), "w");
}
inline int descriptorOf(std::FILE* stream) {
    return fileno(stream);
}
inline int duplicateDescriptor(int descriptor) {
    return dup(descriptor);
}
inline int redirectDescriptor(int from, int onto) {
    return dup2(from, onto);
}
inline int closeDescriptor(int descriptor) {
    return close(descriptor);
}
#endif
} // namespace error_stream

/** Runs `run` with the error stream sent to a scratch file, then reads the log's warnings back
 * from it. What `run` throws is kept, not thrown on. */
inline LoggedRun logWhile(const std::function<void()>& run) {
    const auto file = scratchDirectory("log-capture") / "errors.txt";
    LoggedRun logged;
    {
        const std::unique_ptr<std::FILE, error_stream::FileCloser> sink{
            error_stream::openForWriting(file)};
        REQUIRE(sink != nullptr);
        std::fflush(stderr);
        const int errors = error_stream::descriptorOf(stderr);
        const int saved = error_stream::duplicateDescriptor(errors);
        REQUIRE(saved >= 0);
        REQUIRE(error_stream::redirectDescriptor(error_stream::descriptorOf(sink.get()), errors) >=
                0);
        try {
            run();
        } catch (...) {
            logged.failure = std::current_exception();
        }
        std::fflush(stderr);
        const bool restored = error_stream::redirectDescriptor(saved, errors) >= 0;
        const bool closed = error_stream::closeDescriptor(saved) == 0;
        REQUIRE(restored);
        REQUIRE(closed);
    }
    constexpr std::string_view kWarningTag = "] WARN  ";
    const std::string text = readTextFile(file);
    const std::string_view lines{text};
    for (usize start = 0; start < lines.size();) {
        const usize end = std::min(lines.find('\n', start), lines.size());
        std::string_view line = lines.substr(start, end - start);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (const usize tag = line.find(kWarningTag); tag != std::string_view::npos) {
            logged.warnings.emplace_back(line.substr(tag + kWarningTag.size()));
        }
        start = end + 1;
    }
    return logged;
}

/** The log's warnings while `run` ran; what `run` throws is thrown on. */
inline std::vector<std::string> warningsWhile(const std::function<void()>& run) {
    LoggedRun logged = logWhile(run);
    if (logged.failure != nullptr) {
        std::rethrow_exception(logged.failure);
    }
    return std::move(logged.warnings);
}

} // namespace gdl::test
