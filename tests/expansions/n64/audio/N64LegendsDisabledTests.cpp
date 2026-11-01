#include <array>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"

#include "expansions/n64/audio/LegendsSoundTable.h"
#include "expansions/n64/audio/N64LegendsEffects.h"
#include "expansions/n64/audio/N64LegendsStream.h"

TEST_CASE("Disabled native music and effects expose unavailable adapters without dependencies",
          "[legends][audio][disabled-audio]") {
    using namespace gdl;
    N64LegendsStream music;
    CHECK_FALSE(music.open({}, 0));
    CHECK_FALSE(music.opened());
    CHECK(music.desc().sampleRate == 0);
    CHECK(music.desc().channels == 0);
    std::vector<f32> samples{1, 2};
    CHECK_FALSE(music.read(samples, 128));
    CHECK(samples == std::vector<f32>{1, 2});
    music.rewind();
    CHECK_FALSE(music.opened());

    N64LegendsEffects effects;
    CHECK_FALSE(effects.open({}));
    CHECK_FALSE(effects.opened());
    CHECK_FALSE(effects.contains(0));
    CHECK(effects.cueIds().empty());
    CHECK_FALSE(effects.create(0));
}

TEST_CASE("Native sound metadata remains available without the optional synthesizer",
          "[legends][audio][disabled-audio]") {
    using namespace gdl;
    using namespace gdl::game;
    const std::array rows{LegendsSoundEntry{.name = "s_test"},
                           LegendsSoundEntry{.name = "s_test_extra"},
                           LegendsSoundEntry{.name = "S_TEST"}};
    CHECK(legendsSoundToken(rows, "S_TEST") == 0x10000U);
    CHECK(legendsSoundToken(rows, "s_test_extra") == 0x10001U);
    CHECK_FALSE(legendsSoundToken(rows, "s_te"));
    CHECK_FALSE(legendsSoundToken(rows, "missing"));
    CHECK_FALSE(legendsSoundToken(rows, ""));
    CHECK_FALSE(legendsSoundToken({}, "s_test"));
    CHECK_THROWS_AS(decodeLegendsSoundTable({}), FormatError);
}
