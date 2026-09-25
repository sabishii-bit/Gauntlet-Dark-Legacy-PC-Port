
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Bosses.h"
#include "game/screens/BossMeter.h"

namespace {

using namespace gdl;
using namespace gdl::game;

HealthMeterDefinition lichMeter() {
    HealthMeterDefinition meter;
    meter.pieces = 2;
    meter.advance = 256;
    meter.leftInset = 44;
    meter.rightInset = 53;
    meter.shown = true;
    meter.backed = true;
    return meter;
}

TEST_CASE("multipart meters retain independent eased health and clear every layer",
          "[game][screens][hud]") {
    std::array<HealthMeterReading, 3> readings;
    for (auto& reading : readings) {
        reading.definition = lichMeter();
        reading.health = 1200;
        reading.maxHealth = 1200;
    }
    readings[1].definition.backed = false;
    readings[2].definition.backed = false;
    readings[1].health = readings[1].maxHealth = 1500;
    BossMeters meters;
    REQUIRE_FALSE(meters.bound());
    meters.bind(readings, nullptr);
    REQUIRE(meters.count() == 3);
    REQUIRE(meters.showing());
    readings[1].health = 0;
    meters.update(2, readings, true, false);
    REQUIRE(meters.meter(0).shown() == 1200);
    REQUIRE(meters.meter(1).shown() == 1494);
    REQUIRE(meters.meter(2).shown() == 1200);
    meters.update(500, readings, true, false);
    REQUIRE(meters.meter(1).shown() == 0);
    REQUIRE(meters.meter(1).showing()); // Empty head fill does not remove the shared frame.
    meters.update(2, {}, false, false);
    for (usize index = 0; index < meters.count(); ++index) {
        REQUIRE_FALSE(meters.meter(index).showing());
    }
    meters.clear();
    REQUIRE_FALSE(meters.bound());
    REQUIRE_FALSE(meters.showing());
    meters.bind(std::span{readings}.first(1), nullptr);
    REQUIRE(meters.count() == 1);
    REQUIRE(meters.meter(0).shown() == 1200);
    REQUIRE_FALSE(meters.meter(1).bound());
}

TEST_CASE("Chimera draws its three head fills on one shared frame and drains the struck head",
          "[game][screens][hud][chimera][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/CHIMERA.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/CHIMERA/animations.json");
    test::unpackedOrSkip("MONSTERS/CHIMERA/textures.json");
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'A');
    REQUIRE(bosses.spawn(35, Vec3{0}, 0));
    auto readings = bosses.healthMeters();
    REQUIRE(readings.size() == 3);
    REQUIRE(readings[0].definition.name == "EAGLE");
    REQUIRE(readings[1].definition.name == "LION");
    REQUIRE(readings[2].definition.name == "SNAKE");
    REQUIRE(readings[0].definition.backed);
    REQUIRE_FALSE(readings[1].definition.backed);
    REQUIRE_FALSE(readings[2].definition.backed);
    REQUIRE(readings[0].maxHealth == 1200);
    REQUIRE(readings[1].maxHealth == 1500);
    REQUIRE(readings[2].maxHealth == 1200);
    auto& textures = bosses.archive()->textures;
    BossMeters meters;
    meters.bind(readings, &textures);
    Canvas canvas;
    const auto draw = [&] {
        device.draws.clear();
        canvas.begin(device, Mat4{1});
        meters.draw(canvas, device);
        canvas.end();
    };
    draw();
    const std::array names{"EAGLE_METER_BG1", "EAGLE_METER_FG1", "EAGLE_METER_BG2",
                           "EAGLE_METER_FG2", "LION_METER_FG1",  "LION_METER_FG2",
                           "SNAKE_METER_FG1", "SNAKE_METER_FG2"};
    const std::array lefts{0, 0, 256, 256, 0, 256, 0, 256};
    REQUIRE(device.draws.size() == names.size());
    for (usize i = 0; i < names.size(); ++i) {
        INFO(names[i]);
        const auto texture = textures.find(names[i]);
        REQUIRE(texture);
        CHECK(device.draws[i].texture == &textures.texture(device, *texture));
        CHECK(test::minCorner(device.draws[i]) == Vec2{lefts[i], BossMeter::kY});
    }
    EnemyHit hit;
    hit.damage = 602; // Armor leaves six hundred damage, directed at the lion.
    bosses.hurt(hit, 2);
    readings = bosses.healthMeters();
    REQUIRE(readings[0].health == 1200);
    REQUIRE(readings[1].health == 900);
    REQUIRE(readings[2].health == 1200);
    meters.update(200, readings, true, false);
    draw();
    REQUIRE(device.draws.size() == names.size());
    CHECK(test::maxCorner(device.draws[5]).x < test::maxCorner(device.draws[7]).x);
    CHECK(meters.meter(0).fillWidths() == meters.meter(2).fillWidths());
    hit.damage = 2000;
    bosses.hurt(hit, 1); // Losing the eagle must not remove its shared background.
    readings = bosses.healthMeters();
    REQUIRE(readings.size() == 3);
    meters.update(500, readings, true, false);
    draw();
    REQUIRE(device.draws.size() == names.size() - 1); // Eagle's empty second strip is omitted.
    CHECK(device.draws.front().texture == &textures.texture(device, *textures.find(names[0])));
    hit.damage = 10000;
    bosses.hurt(hit);
    meters.update(2, bosses.healthMeters(), bosses.view().alive, false);
    draw();
    REQUIRE(device.draws.empty());
    meters.clear();
    bosses.close();
}

TEST_CASE("a boss meter fills its two strips by the original's arithmetic and eases toward "
          "the boss's health",
          "[game][screens][hud]") {
    BossMeter meter;
    REQUIRE_FALSE(meter.bound());
    REQUIRE(meter.fillWidths() == std::array<s32, 2>{0, 0});
    const HealthMeterDefinition none;
    REQUIRE_FALSE(meter.bind(none, nullptr));
    REQUIRE(meter.bind(lichMeter(), nullptr));
    REQUIRE(meter.bound());
    // The first health is taken as it is: full, both strips filled to the second's tail.
    meter.update(2, 3000.0f, 3000.0f, true, false);
    REQUIRE(meter.shown() == 3000.0f);
    REQUIRE(meter.showing());
    REQUIRE(meter.fillWidths() == std::array<s32, 2>{256, 256 - 53});
    // Half: the first strip whole, the second empty.
    meter.update(0, 1500.0f, 3000.0f, true, false);
    for (s32 i = 0; i < 300; ++i) {
        meter.update(2, 1500.0f, 3000.0f, true, false);
    }
    REQUIRE(meter.shown() == 1500.0f);
    REQUIRE(meter.fillWidths() == std::array<s32, 2>{256, 0});
    // A quarter: the first strip half of what lies past its cap of 44.
    for (s32 i = 0; i < 300; ++i) {
        meter.update(2, 750.0f, 3000.0f, true, false);
    }
    REQUIRE(meter.fillWidths() == std::array<s32, 2>{44 + (256 - 44) / 2, 0});
    // A blow shows at three a tick, not at once; healing climbs the same way.
    meter.update(2, 150.0f, 3000.0f, true, false);
    REQUIRE(meter.shown() == 744.0f);
    meter.update(2, 900.0f, 3000.0f, true, false);
    REQUIRE(meter.shown() == 750.0f);
    // Nothing shows under nought, and the meter goes with the boss.
    for (s32 i = 0; i < 400; ++i) {
        meter.update(2, -50.0f, 3000.0f, true, false);
    }
    REQUIRE(meter.shown() == 0.0f);
    REQUIRE(meter.fillWidths() == std::array<s32, 2>{44, 0}); // the cap is not fill
    meter.update(2, 0.0f, 3000.0f, false, false);
    REQUIRE_FALSE(meter.showing());
    meter.clear();
    REQUIRE_FALSE(meter.bound());
    // One strip runs from its cap to its tail.
    HealthMeterDefinition single = lichMeter();
    single.pieces = 1;
    REQUIRE(meter.bind(single, nullptr));
    meter.update(2, 3000.0f, 3000.0f, true, false);
    REQUIRE(meter.fillWidths() == std::array<s32, 2>{256 - 44 - 53, 0});
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
