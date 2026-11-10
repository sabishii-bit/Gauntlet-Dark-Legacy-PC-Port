// What HeadlessDebugReports.cpp leaves installed in a Windows Debug test program; elsewhere
// there is nothing to check.
#if defined(_MSC_VER) && defined(_DEBUG)

#include <csignal>
#include <cstdlib>
#include <initializer_list>

#include <catch2/catch_test_macros.hpp>
#include <crtdbg.h>
#include <windows.h>

namespace {

TEST_CASE("Debug test programs report runtime failures on stderr instead of in dialogs",
          "[test-support][debug-reports]") {
    constexpr UINT kSilent =
        SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX;
    CHECK((GetErrorMode() & kSilent) == kSilent);
    CHECK((_set_abort_behavior(0, 0) & (_WRITE_ABORT_MSG | _CALL_REPORTFAULT)) == 0);
    CHECK(_get_invalid_parameter_handler() != nullptr);
    const auto abortHandler = std::signal(SIGABRT, SIG_GET);
    CHECK(abortHandler != SIG_DFL);
    CHECK(abortHandler != SIG_ERR);
    for (const int type : {_CRT_ERROR, _CRT_ASSERT}) {
        CAPTURE(type);
        CHECK((_CrtSetReportMode(type, _CRTDBG_REPORT_MODE) & _CRTDBG_MODE_WNDW) == 0);
        // The runtime keeps the handle _CRTDBG_FILE_STDERR stands for.
        CHECK(_CrtSetReportFile(type, _CRTDBG_REPORT_FILE) == GetStdHandle(STD_ERROR_HANDLE));
    }
}

} // namespace

#endif
