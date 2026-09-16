#include <catch2/catch_test_macros.hpp>

#include "engine/core/Assert.h"

namespace {

int evaluations = 0;

bool countingCheck() {
    ++evaluations;
    return true;
}

TEST_CASE("passing assertions evaluate their condition exactly once", "[core][assert]") {
    evaluations = 0;
    GDL_VERIFY(countingCheck(), "never fires");
    REQUIRE(evaluations == 1);
}

TEST_CASE("GDL_ASSERT is active only in Debug builds", "[core][assert]") {
    evaluations = 0;
    GDL_ASSERT(countingCheck(), "never fires");
    REQUIRE(evaluations == (GDL_DEBUG != 0 ? 1 : 0));
}

} // namespace
