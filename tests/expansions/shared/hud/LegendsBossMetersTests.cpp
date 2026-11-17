#include <array>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"

#include "FakeRenderDevice.h"
#include "HudLookArt.h"
#include "RecordedQuads.h"
#include "expansions/shared/hud/LegendsBossMeters.h"
#include "game/hud/BossMeter.h"
#include "game/hud/HealthMeter.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using test::checkRect;
using test::Quad;

/** One quad a meter should draw. */
struct Piece {
    const Texture* texture = nullptr;
    Rect area;
    Rect uv;
};

/** The quads `meter` draws with strips `strip` across at `top`, `widths` and `shade`. */
std::vector<Quad> drawn(test::FakeRenderDevice& device, const BossMeter& meter, f32 strip, f32 top,
                        std::span<const s32> widths, Color shade) {
    device.draws.clear();
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    drawLegendsBossMeter(canvas, device, meter, strip, top, widths, shade);
    canvas.end();
    return test::quadsOf(device);
}

/** Checks `quads` are `expected`, in turn, each under `shade`. */
void check(const std::vector<Quad>& quads, const std::vector<Piece>& expected, Color shade) {
    REQUIRE(quads.size() == expected.size());
    for (usize index = 0; index < expected.size(); ++index) {
        CAPTURE(index);
        CHECK(quads[index].texture == expected[index].texture);
        checkRect(quads[index].area, expected[index].area);
        checkRect(quads[index].uv, expected[index].uv);
        CHECK(quads[index].color == shade);
    }
}

TEST_CASE("a Legends look's boss meter stands each strip's back whole and crops its fill",
          "[legends][hud-meters]") {
    test::FakeRenderDevice device;
    TextureSet art = test::meterArt(BossMeter::kPieceWidth, 64);
    std::array<HealthMeterReading, 1> boss{test::meterReading(2, 46, 72, 1875, 2500)};
    BossMeters meters;
    meters.bind(boss, &art);
    REQUIRE(meters.count() == 1);
    const BossMeter& meter = meters.meter(0);
    const auto left = static_cast<f32>(meter.layout().left);
    const auto picture = [&](std::string_view name) {
        return test::meterPicture(art, device, name);
    };
    const Color shade = Color::white().withAlpha(200);

    // Strips 200 across from the meter's left at 1, each back whole; the first fill cropped to
    // 150, its picture's left three quarters, not squeezed; the second, of no width, not drawn.
    constexpr std::array<s32, 2> kWidths{150, 0};
    check(drawn(device, meter, 200, 1, kWidths, shade),
          {{picture("METER_BG1"), {left, 1, 200, 64}, {0, 0, 1, 1}},
           {picture("METER_FG1"), {left, 1, 150, 64}, {0, 0, 0.75f, 1}},
           {picture("METER_BG2"), {left + 200, 1, 200, 64}, {0, 0, 1, 1}}},
          shade);

    // A width is kept to its strip: past it the fill is whole, under nought it is not drawn.
    constexpr std::array<s32, 2> kOver{300, -5};
    check(drawn(device, meter, 200, 1, kOver, shade),
          {{picture("METER_BG1"), {left, 1, 200, 64}, {0, 0, 1, 1}},
           {picture("METER_FG1"), {left, 1, 200, 64}, {0, 0, 1, 1}},
           {picture("METER_BG2"), {left + 200, 1, 200, 64}, {0, 0, 1, 1}}},
          shade);

    // A strip `widths` has no width for shows its back alone.
    constexpr std::array<s32, 1> kFirstOnly{64};
    check(drawn(device, meter, 256, 8, kFirstOnly, Color::white()),
          {{picture("METER_BG1"), {left, 8, 256, 64}, {0, 0, 1, 1}},
           {picture("METER_FG1"), {left, 8, 64, 64}, {0, 0, 0.25f, 1}},
           {picture("METER_BG2"), {left + 256, 8, 256, 64}, {0, 0, 1, 1}}},
          Color::white());

    // A meter whose type has no backs draws its fills alone.
    boss[0].definition.backed = false;
    BossMeters bare;
    bare.bind(boss, &art);
    REQUIRE(bare.count() == 1);
    const auto bareLeft = static_cast<f32>(bare.meter(0).layout().left);
    check(drawn(device, bare.meter(0), 256, 8, kWidths, shade),
          {{picture("METER_FG1"), {bareLeft, 8, 150, 64}, {0, 0, 150.0f / 256, 1}}}, shade);
}

} // namespace
