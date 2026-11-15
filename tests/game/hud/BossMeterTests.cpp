#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/bosses/Bosses.h"
#include "game/hud/BossMeter.h"

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
          "[game][screens][hud][chimera][assets]") {
    const auto root = test::assetOrSkip("CRITTER/CHIMERA.WAD").parent_path().parent_path();
    test::assetOrSkip("MONSTERS/CHIMERA/ANIM.PS2");
    test::assetOrSkip("MONSTERS/CHIMERA/textures.ngc");
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
    bosses.wake();
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

TEST_CASE("a bound meter tells its boss art, pieces, origin and insets", "[game][screens][hud]") {
    // A boss's own two strips, a head of one strip over a background of its own, and a head
    // that shares that background's origin.
    std::vector<TextureSetEntry> entries;
    std::vector<Image> images;
    for (const char* name : {"METER_BG1", "METER_FG1", "METER_BG2", "METER_FG2", "EAGLE_METER_BG1",
                             "EAGLE_METER_FG1", "LION_METER_BG1", "LION_METER_FG1"}) {
        entries.push_back({.name = name, .width = 256, .height = 16, .file = {}});
        images.push_back(Image::filled(256, 16, Color::white()));
    }
    auto art = TextureSet::fromDecoded(std::move(entries), std::move(images));
    std::array<HealthMeterReading, 3> readings;
    readings[0].definition = lichMeter();
    readings[0].health = 750; // a quarter: the first strip's fill cropped, the second's none
    readings[0].maxHealth = 3000;
    readings[1].definition = lichMeter();
    readings[1].definition.name = "EAGLE";
    readings[1].definition.pieces = 1;
    readings[1].definition.leftInset = 12;
    readings[1].definition.rightInset = 20;
    readings[1].health = readings[1].maxHealth = 500;
    readings[2].definition = readings[1].definition;
    readings[2].definition.name = "LION";
    readings[2].definition.backed = false;
    readings[2].health = readings[2].maxHealth = 500;
    BossMeters meters;
    meters.bind(readings, &art);
    REQUIRE(meters.count() == 3);
    const BossMeter::Layout boss{.textures = &art,
                                 .name = "",
                                 .pieces = 2,
                                 .left = 0,
                                 .leftInset = 44,
                                 .rightInset = 53,
                                 .backed = true};
    const BossMeter::Layout eagle{.textures = &art,
                                  .name = "EAGLE",
                                  .pieces = 1,
                                  .left = 256,
                                  .leftInset = 12,
                                  .rightInset = 20,
                                  .backed = true};
    BossMeter::Layout lion = eagle;
    lion.name = "LION";
    lion.backed = false;
    CHECK(meters.meter(0).layout() == boss);
    CHECK(meters.meter(1).layout() == eagle);
    CHECK(meters.meter(2).layout() == lion);

    test::FakeRenderDevice device;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    meters.draw(canvas, device);
    canvas.end();
    const u32 created = device.texturesCreated;
    const auto picture = [&](std::string_view name) {
        const auto index = art.find(name);
        REQUIRE(index.has_value());
        return &art.texture(device, *index);
    };
    using Part = BossMeter::MeterPart;
    // The boss's backgrounds and its first fill, cropped; then the eagle's background and fill,
    // and the lion's fill over the eagle's background.
    REQUIRE(device.draws.size() == 6);
    const Texture* fill = meters.meter(0).pieceTexture(device, Part::Fill, 0);
    CHECK(fill == picture("METER_FG1"));
    CHECK(device.draws[1].texture == fill);
    CHECK(test::minCorner(device.draws[1]) == Vec2{0.0f, BossMeter::kY});
    CHECK(test::maxCorner(device.draws[1]) == Vec2{150.0f, BossMeter::kY + 16.0f});
    f32 across = 0;
    for (const ImmediateVertex& vertex : device.draws[1].vertices) {
        across = std::max(across, vertex.uv.x);
    }
    CHECK(across == 150.0f / 256.0f);
    CHECK(device.draws[0].texture == meters.meter(0).pieceTexture(device, Part::Background, 0));
    CHECK(device.draws[0].texture == picture("METER_BG1"));
    CHECK(device.draws[2].texture == meters.meter(0).pieceTexture(device, Part::Background, 1));
    CHECK(test::minCorner(device.draws[2]) == Vec2{256.0f, BossMeter::kY});
    CHECK(device.draws[3].texture == meters.meter(1).pieceTexture(device, Part::Background, 0));
    CHECK(device.draws[3].texture == picture("EAGLE_METER_BG1"));
    CHECK(test::minCorner(device.draws[3]) == Vec2{256.0f, BossMeter::kY});
    CHECK(device.draws[4].texture == meters.meter(1).pieceTexture(device, Part::Fill, 0));
    CHECK(device.draws[5].texture == meters.meter(2).pieceTexture(device, Part::Fill, 0));
    CHECK(device.draws[5].texture == picture("LION_METER_FG1"));
    CHECK(test::minCorner(device.draws[5]) == Vec2{256.0f, BossMeter::kY});
    CHECK(device.texturesCreated == created);
    // A strip drawn empty still has its fill; none past a meter's strips, and no background
    // where the meter draws none.
    CHECK(meters.meter(0).pieceTexture(device, Part::Fill, 1) == picture("METER_FG2"));
    CHECK(meters.meter(0).pieceTexture(device, Part::Fill, 2) == nullptr);
    CHECK(meters.meter(0).pieceTexture(device, Part::Background, -1) == nullptr);
    CHECK(meters.meter(1).pieceTexture(device, Part::Fill, 1) == nullptr);
    CHECK(meters.meter(2).pieceTexture(device, Part::Background, 0) == nullptr);

    meters.clear();
    for (usize index = 0; index < BossMeters::kCapacity; ++index) {
        CAPTURE(index);
        CHECK(meters.meter(index).layout() == BossMeter::Layout{});
        CHECK(meters.meter(index).pieceTexture(device, Part::Fill, 0) == nullptr);
    }
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
          "[game][screens][hud][assets]") {
    const std::filesystem::path archiveDirectory =
        test::assetOrSkip("MONSTERS/LICH/textures.ngc").parent_path();
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

TEST_CASE("native boss meter rewind restores independent easing after wrong health branches",
          "[boss-meters-local-rewind][native-equality]") {
    std::array<HealthMeterReading, 3> readings;
    for (auto& r : readings) {
        r.definition = lichMeter();
        r.health = r.maxHealth = 1200;
    }
    BossMeters reference;
    BossMeters predicted;
    reference.bind(readings, nullptr);
    predicted.bind(readings, nullptr);
    BossMeters::LocalSnapshot saved;
    std::array<BossMeters::LocalSnapshot, 8> expected;
    BossMeters::LocalSnapshot actual;
    const auto advance = [&](BossMeters& meters, s32 tick) {
        for (usize i = 0; i < readings.size(); ++i) {
            readings[i].health = static_cast<f32>(200 + (tick * (13 + i)) % 900);
        }
        meters.update(1, readings, tick < 180, tick % 9 < 3);
    };
    s32 tick = 0;
    for (s32 window = 0; window < 48; ++window) {
        predicted.saveLocal(saved);
        const s32 distance = 1 + window % 8;
        for (s32 step = 0; step < distance; ++step) {
            advance(predicted, tick + step);
            predicted.saveLocal(expected[static_cast<usize>(step)]);
        }
        REQUIRE(predicted.restoreLocal(saved));
        auto wrong = readings;
        wrong[0].health = 0;
        wrong[1].maxHealth = 5000;
        for (s32 step = 0; step < distance; ++step) {
            predicted.update(2, wrong, false, true);
        }
        REQUIRE(predicted.restoreLocal(saved));
        for (s32 step = 0; step < distance; ++step, ++tick) {
            advance(reference, tick);
            advance(predicted, tick);
            predicted.saveLocal(actual);
            CHECK(predicted.equalLocal(expected[static_cast<usize>(step)], actual));
            for (usize i = 0; i < readings.size(); ++i) {
                CHECK(predicted.meter(i).shown() == reference.meter(i).shown());
                CHECK(predicted.meter(i).look().visible == reference.meter(i).look().visible);
                CHECK(predicted.meter(i).look().frozen == reference.meter(i).look().frozen);
                CHECK(predicted.meter(i).fillWidths() == reference.meter(i).fillWidths());
            }
        }
        REQUIRE(predicted.canRestoreLocal(saved));
    }
    CHECK_FALSE(reference.showing());
}

TEST_CASE("native boss meter histories reject foreign layouts and preserve first-health state",
          "[boss-meters-local-rewind][native-equality]") {
    BossMeter meter;
    BossMeter foreign;
    REQUIRE(meter.bind(lichMeter(), nullptr));
    REQUIRE(foreign.bind(lichMeter(), nullptr));
    BossMeter::LocalSnapshot saved;
    CHECK_FALSE(meter.equalLocal(saved, saved));
    CHECK_FALSE(meter.restoreLocal(saved));
    meter.saveLocal(saved);
    CHECK(meter.equalLocal(saved, saved));
    CHECK_FALSE(foreign.equalLocal(saved, saved));
    CHECK_FALSE(foreign.restoreLocal(saved));
    meter.update(1, 5000, 5000, false, true);
    REQUIRE(meter.restoreLocal(saved));
    meter.update(1, 700, 1000, true, false);
    CHECK(meter.shown() == 700);
    REQUIRE(meter.restoreLocal(saved));
    REQUIRE(meter.bind(lichMeter(), nullptr));
    CHECK_FALSE(meter.equalLocal(saved, saved));
    CHECK_FALSE(meter.restoreLocal(saved));
    BossMeters many;
    BossMeters other;
    BossMeters::LocalSnapshot group;
    many.saveLocal(group);
    CHECK(many.equalLocal(group, group));
    CHECK_FALSE(other.equalLocal(group, group));
    CHECK_FALSE(other.restoreLocal(group));
    REQUIRE(many.restoreLocal(group));
    many.clear();
    CHECK_FALSE(many.equalLocal(group, group));
    CHECK_FALSE(many.restoreLocal(group));
}

TEST_CASE("native boss meter equality detects hidden first-update state and maximum health",
          "[boss-meters-local-rewind][native-equality]") {
    BossMeter meter;
    REQUIRE(meter.bind(lichMeter(), nullptr));
    BossMeter::LocalSnapshot before;
    BossMeter::LocalSnapshot after;
    meter.saveLocal(before);
    const auto look = meter.look();
    meter.update(0, 0, 1, true, false);
    meter.saveLocal(after);
    CHECK(meter.look().visible == look.visible);
    CHECK(meter.look().widths == look.widths);
    CHECK_FALSE(meter.equalLocal(before, after)); // first-health latch only
    meter.saveLocal(before);
    meter.update(0, 0, 2, true, false);
    meter.saveLocal(after);
    CHECK(meter.look().widths == look.widths);
    CHECK_FALSE(meter.equalLocal(before, after)); // maximum with no visible fill
}

} // namespace
