#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PortalDeparture.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("portal departure sinks and spins for fifty ticks even without artwork",
          "[portal-departure]") {
    test::FakeRenderDevice device;
    TextureSet empty;
    PortalDeparture departure;
    const Mat4 body = glm::translate(Mat4{1}, Vec3{20, 8, -5});
    CHECK(departure.transform(body) == body);
    departure.begin(device, empty);
    departure.update(25);
    const auto middle = departure.transform(body);
    CHECK(middle[3].x == 20);
    CHECK(middle[3].y == Approx(5));
    CHECK(middle[3].z == -5);
    CHECK(middle[0].x == Approx(std::cos(PortalDeparture::kSpinPerSecond * 25 / 60)));
    CHECK_FALSE(departure.finished());
    departure.update(25);
    CHECK(departure.finished());
    CHECK(departure.transform(body)[3].y == Approx(2));
    departure.update(1000);
    CHECK(departure.transform(body)[3].y == Approx(2));
    departure.clear();
    CHECK_FALSE(departure.started());
    CHECK(departure.transform(body) == body);
}

TEST_CASE("portal departure uses the animated lightning skin, not spawn flames",
          "[portal-departure][unpacked]") {
    const auto path = test::unpackedOrSkip("WEAPONS/textures.json");
    test::FakeRenderDevice device;
    TextureSet textures;
    REQUIRE(textures.load(path.parent_path()));
    PortalDeparture departure;
    departure.begin(device, textures);
    const auto first = textures.find("DTH_LIGHT00");
    const auto last = textures.find("DTH_LIGHT00+9");
    REQUIRE(first);
    REQUIRE(last);
    CHECK(departure.skin() == &textures.texture(device, *first));
    departure.update(45);
    CHECK(departure.skin() == &textures.texture(device, *last));
    departure.update(5);
    CHECK(departure.skin() == nullptr);
    departure.clear();
}
} // namespace
