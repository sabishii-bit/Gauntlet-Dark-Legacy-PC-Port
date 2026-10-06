#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ObjectMaterial.h"

namespace {
using namespace gdl;
TEST_CASE("object material bits keep depth compare independent from blending and sorting",
          "[asset-conformance][object-material]") {
    for (u32 bits = 0; bits < 32; ++bits) {
        const u32 flags = ((bits & 1U) != 0 ? 0x800000U : 0U) | ((bits & 2U) != 0 ? 0x40U : 0U) |
                          ((bits & 4U) != 0 ? 0x80U : 0U) | ((bits & 8U) != 0 ? 0x800U : 0U) |
                          ((bits & 16U) != 0 ? 0x8000U : 0U);
        const auto material = ObjectMaterial::fromFlags(flags);
        CHECK(material.additive == ((bits & 1U) != 0));
        CHECK(material.depthTest == ((bits & 2U) == 0));
        CHECK(material.depthWrite == ((bits & 4U) == 0));
        CHECK(material.sorted == ((bits & 8U) != 0));
        CHECK(material.chrome == ((bits & 16U) != 0));
    }
    const auto ghost = ObjectMaterial::fromFlags(0xC01880);
    CHECK(ghost.additive);
    CHECK(ghost.depthTest);
    CHECK_FALSE(ghost.depthWrite);
    CHECK(ghost.sortBias == -20000);
    CHECK(ObjectMaterial::fromFlags(0x80000).sortBias == -10000);
    CHECK(ObjectMaterial::fromFlags(0x480000).sortBias == -20000);
    CHECK(ObjectMaterial::fromFlags(0x04000000).facing == 4);
    CHECK(ObjectMaterial::fromFlags(0x08000000).facing == 8);
}
} // namespace
