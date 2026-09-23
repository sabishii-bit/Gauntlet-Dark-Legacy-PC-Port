#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/BossMeter.h"

namespace {

using namespace gdl;
using namespace gdl::game;

CritterMeter lichMeter() {
    CritterMeter meter;
    meter.pieces = 2;
    meter.advance = 256;
    meter.leftInset = 44;
    meter.rightInset = 53;
    meter.shown = true;
    meter.backed = true;
    return meter;
}

TEST_CASE("a boss meter fills its two strips by the original's arithmetic and eases toward "
          "the boss's health",
          "[game][screens][hud]") {
    BossMeter meter;
    REQUIRE_FALSE(meter.bound());
    REQUIRE(meter.fillWidths() == std::array<std::int32_t, 2>{0, 0});
    const CritterMeter none;
    REQUIRE_FALSE(meter.bind(none, nullptr));
    REQUIRE(meter.bind(lichMeter(), nullptr));
    REQUIRE(meter.bound());
    // The first health is taken as it is: full, both strips filled to the second's tail.
    meter.update(2, 3000.0f, 3000.0f, true, false);
    REQUIRE(meter.shown() == 3000.0f);
    REQUIRE(meter.showing());
    REQUIRE(meter.fillWidths() == std::array<std::int32_t, 2>{256, 256 - 53});
    // Half: the first strip whole, the second empty.
    meter.update(0, 1500.0f, 3000.0f, true, false);
    for (int i = 0; i < 300; ++i) {
        meter.update(2, 1500.0f, 3000.0f, true, false);
    }
    REQUIRE(meter.shown() == 1500.0f);
    REQUIRE(meter.fillWidths() == std::array<std::int32_t, 2>{256, 0});
    // A quarter: the first strip half of what lies past its cap of 44.
    for (int i = 0; i < 300; ++i) {
        meter.update(2, 750.0f, 3000.0f, true, false);
    }
    REQUIRE(meter.fillWidths() == std::array<std::int32_t, 2>{44 + (256 - 44) / 2, 0});
    // A blow shows at three a tick, not at once; healing climbs the same way.
    meter.update(2, 150.0f, 3000.0f, true, false);
    REQUIRE(meter.shown() == 744.0f);
    meter.update(2, 900.0f, 3000.0f, true, false);
    REQUIRE(meter.shown() == 750.0f);
    // Nothing shows under nought, and the meter goes with the boss.
    for (int i = 0; i < 400; ++i) {
        meter.update(2, -50.0f, 3000.0f, true, false);
    }
    REQUIRE(meter.shown() == 0.0f);
    REQUIRE(meter.fillWidths() == std::array<std::int32_t, 2>{44, 0}); // the cap is not fill
    meter.update(2, 0.0f, 3000.0f, false, false);
    REQUIRE_FALSE(meter.showing());
    meter.clear();
    REQUIRE_FALSE(meter.bound());
    // One strip runs from its cap to its tail.
    CritterMeter single = lichMeter();
    single.pieces = 1;
    REQUIRE(meter.bind(single, nullptr));
    meter.update(2, 3000.0f, 3000.0f, true, false);
    REQUIRE(meter.fillWidths() == std::array<std::int32_t, 2>{256 - 44 - 53, 0});
}

TEST_CASE("a boss meter is drawn from the boss's own archive across the top of the screen",
          "[game][screens][hud][unpacked]") {
    const std::filesystem::path archiveDirectory =
        test::unpackedOrSkip("MONSTERS/LICH/textures.json").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(archiveDirectory));
    BossMeter meter;
    REQUIRE(meter.bind(lichMeter(), &archive.textures));
    meter.update(2, 3000.0f, 3000.0f, true, false);
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    meter.draw(canvas, device);
    canvas.end();
    // Two backgrounds and two fills: four textures, in that order, as one batch each.
    REQUIRE(device.draws.size() == 4);
    // Frozen, the same, tinted; dead, nothing.
    device.draws.clear();
    meter.update(2, 3000.0f, 3000.0f, true, true);
    canvas.begin(device, Mat4{1.0f});
    meter.draw(canvas, device);
    canvas.end();
    REQUIRE(device.draws.size() == 4);
    device.draws.clear();
    meter.update(2, 0.0f, 3000.0f, false, false);
    canvas.begin(device, Mat4{1.0f});
    meter.draw(canvas, device);
    canvas.end();
    REQUIRE(device.draws.empty());
    // Without textures nothing is drawn, and nothing is amiss.
    BossMeter bare;
    REQUIRE(bare.bind(lichMeter(), nullptr));
    bare.update(2, 10.0f, 10.0f, true, false);
    canvas.begin(device, Mat4{1.0f});
    bare.draw(canvas, device);
    canvas.end();
    REQUIRE(device.draws.empty());
}

} // namespace
