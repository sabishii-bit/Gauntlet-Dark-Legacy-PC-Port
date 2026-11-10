// Windows Debug runtimes stop a program on a failed assert, an STL debug check, an invalid
// parameter or abort() with a modal dialog, which holds an unattended test run until someone
// answers it. Every test program lists this source, so each reports those on stderr and fails.
#if defined(_MSC_VER) && defined(_DEBUG)

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <crtdbg.h>
#include <windows.h>

namespace {

/** A test program's exit code after a Debug runtime report: a failure, never the 4 that ctest's
 * Catch2 discovery reads as a skip. */
constexpr UINT kReportedExit = 3;

/** Ends the process at once: the failing thread may hold locks an orderly exit would take. */
[[noreturn]] void endReported() {
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), kReportedExit);
    std::_Exit(static_cast<int>(kReportedExit));
}

int reportToStderr(int type, char* message, int* /*returnValue*/) {
    if (type == _CRT_WARN) {
        return FALSE; // the runtime's own handling: debugger output only
    }
    const char* const text = message != nullptr ? message : "";
    const auto length = std::strlen(text);
    std::fprintf(stderr, "Debug runtime %s: %s%s", type == _CRT_ASSERT ? "assertion" : "error",
                 text, length != 0 && text[length - 1] == '\n' ? "" : "\n");
    endReported();
}

void reportInvalidParameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file,
                            unsigned int line, std::uintptr_t /*reserved*/) {
    std::fprintf(stderr, "Debug runtime invalid parameter: %ls in %ls (%ls:%u)\n",
                 expression != nullptr ? expression : L"?", function != nullptr ? function : L"?",
                 file != nullptr ? file : L"?", line);
    endReported();
}

void reportAbort(int /*signal*/) {
    std::fputs("Debug runtime: abort() was called\n", stderr);
    endReported();
}

/** Installs the reports as the program starts, before main runs. */
bool installReports() {
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, reportAbort);
    _set_invalid_parameter_handler(reportInvalidParameter);
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, reportToStderr);
    // A report that still reaches the runtime's own handling goes to stderr, never a window.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    return true;
}

[[maybe_unused]] const bool kReportsInstalled = installReports();

} // namespace

#endif
