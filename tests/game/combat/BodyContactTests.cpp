#include <catch2/catch_test_macros.hpp>

#include "game/combat/BodyContact.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("body contact covers the entire finite movement segment", "[body-contact][enemies]") {
    CHECK(movementTouchesBody({0, 0, -10}, {0, 0, 10}, {}, 2, 3));
    CHECK(movementTouchesBody({0, 0, 10}, {0, 0, -10}, {}, 2, 3));
    CHECK_FALSE(movementTouchesBody({0, 0, -10}, {0, 0, -3}, {}, 2, 3));
    CHECK_FALSE(movementTouchesBody({3, 0, -10}, {3, 0, 10}, {}, 2, 3));
    CHECK(movementTouchesBody({2, 0, -10}, {2, 0, 10}, {}, 2, 3)); // inclusive tangent
    CHECK(movementTouchesBody({0, 3, -10}, {0, 3, 10}, {}, 2, 3));
    CHECK_FALSE(movementTouchesBody({0, 3.01f, -10}, {0, 3.01f, 10}, {}, 2, 3));
    CHECK(movementTouchesBody({0, 10, -10}, {0, -10, 10}, {}, 2, 3));
    // The retail test projects in 3D even for a tall cylinder. Do not substitute
    // a horizontal-only closest point or a generic cylinder intersection routine.
    CHECK_FALSE(movementTouchesBody({0, 8, -4}, {0, 4, 4}, {}, 1, 8));
}

TEST_CASE("existing body overlaps allow retreat but not an inward or sideways move",
          "[body-contact][enemies]") {
    CHECK(movementTouchesBody({0, 0, -1}, {0, 0, 1}, {}, 2, 3));
    CHECK(movementTouchesBody({0, 0, -1}, {0, 0, 10}, {}, 2, 3));
    CHECK_FALSE(movementTouchesBody({0, 0, -1}, {0, 0, -1.5f}, {}, 2, 3));
    CHECK(movementTouchesBody({0, 0, -1}, {1, 0, -1}, {}, 2, 3));
    CHECK(movementTouchesBody({0, 0, -1}, {1, 0, -1.005f}, {}, 2, 3));
    CHECK_FALSE(movementTouchesBody({0, 0, -1}, {1, 0, -1.02f}, {}, 2, 3));
    CHECK(movementTouchesBody({0, 0, -1}, {0, 0, -1}, {}, 2, 3));
    CHECK(movementTouchesBody({}, {}, {}, 2, 3));
    CHECK_FALSE(movementTouchesBody({}, {0, 0, 1}, {}, 2, 3));
    CHECK(movementTouchesBody({}, {0, 0, 0.0005f}, {}, 2, 3));
}
} // namespace
